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
    static char const roles[] = "UATXNKCR";
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

    // Newlines counted eight bytes at a time agree with a byte at a time,
    // whatever bytes surround them
    {
        Arena a      = perm;
        u64   rng    = 1;
        i32   wrong  = 0;
        u8    pick[] = {'\n', 0x0b, 0x09, 0x8a, 0x00, 0xff, 'a', 0x7f};
        for (i32 n = 0; n < 4000; n++) {
            u8  buf[40];
            iz  len  = n % 41;
            i64 want = 0;
            for (iz i = 0; i < len; i++) {
                rng    = rng*0x3243f6a8885a308du + 1;
                buf[i] = pick[rng>>61];
                want  += buf[i] == '\n';
            }
            wrong += countnl({buf, len}) != want;
        }
        expect(t, "view.countnl", istr(&a, wrong), "0");
    }

    // Edits in the patch and hashline formats: summaries and diffs
    {
        Arena a     = perm;
        Str   patch = "{\"patch\":\"*** Begin Patch\\n*** Update File: a.c\\n@@ f\\n-x\\n+y\\n"
                      "*** Delete File: b.c\\n*** End Patch\"}";
        Arena tmp   = carve(&a, 1<<16);
        expect(t, "view.patch summary", toolsummary(&a, tmp, "edit", patch, "/w"), "a.c, b.c");
        expect(t, "view.patch diff", tooldiff(&a, "edit", patch, "ok"),
               "```diff\n--- a.c\n+++ a.c\n@@ f\n-x\n+y\n--- b.c\n+++ /dev/null\n```\n");
        Str lines = "{\"path\":\"a.c\",\"tag\":\"AB12\",\"edits\":[{\"op\":\"replace\",\"line\":2,"
                    "\"end\":3,\"text\":\"z\\n\"},{\"op\":\"insert\",\"line\":0,\"text\":\"t\"},"
                    "{\"op\":\"delete\",\"line\":9}]}";
        expect(t, "view.hashline diff", tooldiff(&a, "edit", lines, "ok"),
               "```diff\n--- a.c\n+++ a.c\n@@ replace lines 2-3 @@\n+z\n@@ insert after line 0 @@\n"
               "+t\n@@ delete lines 9 @@\n```\n");
    }

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
        expect(t, "view.shellend", v->msgs[1].text, "ok\n");  // the Output action has the file
        expect(t, "view.spill", sh->spill, "out/c1.log");
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

    // Toasts: what asks and turn ends mark, and what they say
    {
        Arena a = perm;
        View *v = testview(&a);
        apply(v, a, R"({"sid":"s","seq":1,"ts":5,"ev":"session","cwd":"/w/proj/"})");
        expect(t, "toast.folder", toasttitle(&a, v, {}), "proj");
        expect(t, "toast.listed", toasttitle(&a, v, "Listed"), "Listed");
        v->nofolder = 1;
        expect(t, "toast.app", toasttitle(&a, v, {}), "Prospero");
        apply(v, a, R"({"sid":"s","seq":2,"ts":6,"ev":"user","text":"go","via":"prompt"})");
        apply(v, a, R"({"sid":"s","seq":3,"ts":7,"ev":"turn","n":1})");
        apply(v, a, R"({"sid":"s","seq":4,"ts":8,"ev":"assistant","turn":1,"attempt":1,"blocks":[)"
                    R"({"t":"call","id":"c1","name":"write","args":"{\"path\":\"/x/y\"}"}]})");
        apply(v, a, R"({"sid":"s","seq":5,"ts":1000,"ev":"ask","req":"c1/ask","call":"c1","kind":"path",)"
                    R"("tool":"write","path":"/x/y","detail":"outside","deadline":31000,"dflt":"deny"})");
        expect(t, "toast.askmark", istr(&a, v->askseq*10000 + v->askts), "51000");
        ViewTool *k = v->msgs[v->askmsg].tool;
        expect(t, "toast.askbody", toastaskbody(&a, k, {}, 1, 1500),
               "Allow write /x/y?\nDenied in 30 s unless allowed");
        expect(t, "toast.askwho", toastaskbody(&a, k, "helper", 1, 40000),
               "helper: Allow write /x/y?\nDenied in 0 s unless allowed");
        expect(t, "toast.asks", toastaskbody(&a, k, {}, 3, 0),
               "3 requests wait for your answer");
        apply(v, a, R"({"sid":"s","seq":6,"ts":1100,"ev":"answer","req":"c1/ask","decision":"allow","by":"user"})");
        apply(v, a, R"({"sid":"s","seq":7,"ts":1200,"ev":"assistant","turn":1,"attempt":2,"blocks":[)"
                    R"({"t":"text","text":"\n  All written.\nMore here."}]})");
        apply(v, a, R"({"sid":"s","seq":8,"ts":1300,"ev":"turn.end","turn":1,"why":"done"})");
        expect(t, "toast.endmark", istr(&a, v->endseq*10 + v->endwhy), "8" "1");
        expect(t, "toast.done", toastdonebody(&a, v), "All written.\xe2\x80\xa6");

        apply(v, a, R"({"sid":"s","seq":9,"ts":1400,"ev":"user","text":"again","via":"prompt"})");
        apply(v, a, R"({"sid":"s","seq":10,"ts":1500,"ev":"turn","n":2})");
        apply(v, a, R"({"sid":"s","seq":11,"ts":1600,"ev":"turn.end","turn":2,"why":"done"})");
        expect(t, "toast.silent", toastdonebody(&a, v), "Finished");
        apply(v, a, R"({"sid":"s","seq":12,"ts":1700,"ev":"turn","n":3})");
        apply(v, a, R"({"sid":"s","seq":13,"ts":1800,"ev":"turn.end","turn":3,"why":"error","err":"HTTP 500\nbody"})");
        expect(t, "toast.error", toastdonebody(&a, v), "Stopped: HTTP 500\xe2\x80\xa6");
        apply(v, a, R"({"sid":"s","seq":14,"ts":1900,"ev":"turn","n":4})");
        apply(v, a, R"({"sid":"s","seq":15,"ts":2000,"ev":"turn.end","turn":4,"why":"budget"})");
        expect(t, "toast.budget", toastdonebody(&a, v), "Stopped at the step budget");
        apply(v, a, R"({"sid":"s","seq":16,"ts":2100,"ev":"turn","n":5})");
        apply(v, a, R"({"sid":"s","seq":17,"ts":2200,"ev":"turn.end","turn":5,"why":"cancel"})");
        expect(t, "toast.cancel", istr(&a, v->endwhy), "0");
        apply(v, a, R"({"sid":"s","seq":18,"ts":2300,"ev":"turn","n":6,"via":"compact"})");
        apply(v, a, R"({"sid":"s","seq":19,"ts":2400,"ev":"turn.end","turn":6,"why":"done"})");
        expect(t, "toast.compact", istr(&a, v->endcompact*10 + v->endresume), "10");
        apply(v, a, R"({"sid":"s","seq":20,"ts":2500,"ev":"turn","n":7})");
        apply(v, a, R"({"sid":"s","seq":21,"ts":2600,"ev":"turn.end","turn":7,"why":"done","resume":true})");
        expect(t, "toast.resume", istr(&a, v->endcompact*10 + v->endresume), "1");

        ViewTool plan = {};
        plan.kind = "plan";
        expect(t, "toast.plan", toastaskbody(&a, &plan, {}, 1, 0),
               "Approve the plan?\nWaiting for you");
        ViewTool mcp = {};
        mcp.kind     = "mcp.project";
        mcp.path     = "/w";
        mcp.deadline = 2000;
        expect(t, "toast.mcp", toastaskbody(&a, &mcp, {}, 1, 1001),
               "Start the MCP servers of /w?\nDenied in 1 s unless allowed");

        expect(t, "notify.empty", istr(&a, notifyparse("")), "0");
        expect(t, "notify.both", istr(&a, notifyparse("allow, done")), "3");
        expect(t, "notify.none", istr(&a, notifyparse("none")), "0");
        expect(t, "notify.odd", istr(&a, notifyparse("done bell")), "2");
        expect(t, "notify.less", istr(&a, notifyparse("allow done -done")), "1");

        Str d = "Seconds without a byte before giving up";
        expect(t, "find.exact", istr(&a, cfgfindrank("timeout", d, "endpoint x", "Timeout")), "1");
        expect(t, "find.prefix", istr(&a, cfgfindrank("timeout_idle", d, "", "time")), "2");
        expect(t, "find.part", istr(&a, cfgfindrank("idle_timeout", d, "", "time")), "3");
        expect(t, "find.inside", istr(&a, cfgfindrank("idletimeout", d, "", "time")), "4");
        expect(t, "find.page", istr(&a, cfgfindrank("url", d, "endpoint falstaff", "falst")), "5");
        expect(t, "find.desc", istr(&a, cfgfindrank("url", d, "endpoint x", "byte")), "6");
        expect(t, "find.none", istr(&a, cfgfindrank("url", d, "endpoint x", "nope")), "0");
        expect(t, "find.empty", istr(&a, cfgfindrank("url", d, "endpoint x", "  ")), "0");
        expect(t, "find.words", istr(&a, cfgfindrank("url", d, "endpoint falstaff", "url falstaff")), "5");
        expect(t, "find.allwords", istr(&a, cfgfindrank("url", d, "endpoint falstaff", "url nope")), "0");
    }

    // Always: the ask offers a grant, the answer allows it for good, and
    // the grant record that follows changes nothing shown; a fork's
    // session record names its original
    {
        Arena a = perm;
        View *v = testview(&a);
        apply(v, a, R"({"sid":"s","seq":1,"ts":0,"ev":"session","forked_from":"o","fork_turn":2,)"
                    R"("cwd":"/w","scratch":"/t/prospero-1/s"})");
        apply(v, a, R"({"sid":"s","seq":2,"ts":0,"ev":"assistant","turn":1,"attempt":1,"blocks":[)"
                    R"({"t":"call","id":"c5","name":"read","args":"{\"path\":\"/x/y\"}"}]})");
        apply(v, a, R"({"sid":"s","seq":3,"ts":0,"ev":"ask","req":"c5/ask","call":"c5","kind":"path",)"
                    R"("tool":"read","path":"/x/y","detail":"outside","always":"/x","access":"read",)"
                    R"("deadline":30000,"dflt":"deny"})");
        ViewTool *k = v->msgs[1].tool;
        expect(t, "view.always", concat(&a, k->grant, concat(&a, " ", k->access)), "/x read");
        apply(v, a, R"({"sid":"s","seq":4,"ts":0,"ev":"answer","req":"c5/ask","decision":"allow",)"
                    R"("by":"user","note":"","always":1})");
        apply(v, a, R"({"sid":"s","seq":5,"ts":0,"ev":"grant","path":"/x","access":"read",)"
                    R"("by":"user","req":"c5/ask"})");
        expect(t, "view.alwaysanswer", istr(&a, v->asks*100 + k->answer*10 + k->always),
               concat(&a, istr(&a, VASK_ALLOWED), "1"));
        expect(t, "view.grantseq", istr(&a, v->lastseq), "5");
        expect(t, "view.fork", concat(&a, v->forkedfrom, concat(&a, " ", istr(&a, v->forkturn))),
               "o 2");
        expect(t, "view.scratch", v->scratch, "/t/prospero-1/s");
    }

    // Plan mode: the mode record sets it, the plan's ask holds the plan
    // in Markdown, and the plan file's changes bump planver
    {
        Arena a = perm;
        View *v = testview(&a);
        apply(v, a, R"({"sid":"s","seq":1,"ts":0,"ev":"session","cwd":"/w"})");
        apply(v, a, R"({"sid":"s","seq":2,"ts":0,"ev":"mode","mode":"plan","by":"new",)"
                    R"("path":"/s/plan.md"})");
        expect(t, "view.mode", concat(&a, istr(&a, v->plan), concat(&a, " ", v->planpath)),
               "1 /s/plan.md");
        i32 ver = v->planver;
        apply(v, a, R"({"sid":"s","seq":3,"ts":0,"ev":"assistant","turn":1,"attempt":1,"blocks":[)"
                    R"({"t":"call","id":"p1","name":"write","args":"{\"path\":\"/s/plan.md\"}"},)"
                    R"({"t":"call","id":"p2","name":"exit_plan_mode","args":"{}"}]})");
        apply(v, a, R"({"sid":"s","seq":4,"ts":0,"ev":"tool.start","call":"p1","name":"write",)"
                    R"("args":"{\"path\":\"/s/plan.md\"}","cwd":"/w"})");
        apply(v, a, R"({"sid":"s","seq":5,"ts":0,"ev":"tool.end","call":"p1","status":"ok","text":"Created."})");
        expect(t, "view.planwrite", istr(&a, v->planver - ver), "1");
        apply(v, a, R"({"sid":"s","seq":6,"ts":0,"ev":"ask","req":"p2/ask","call":"p2","kind":"plan",)"
                    R"("tool":"exit_plan_mode","path":"/s/plan.md","detail":"# Plan\n\n- One","deadline":0})");
        ViewMsg *m = &v->msgs[v->msgs.len-1];
        expect(t, "view.planask", concat(&a, m->tool->kind, istr(&a, m->md)), "plan1");
        expect(t, "view.planver", istr(&a, v->planver - ver), "2");
        expect(t, "view.plandeadline", istr(&a, m->tool->deadline), "0");
        apply(v, a, R"({"sid":"s","seq":7,"ts":0,"ev":"answer","req":"p2/ask","decision":"allow","by":"user"})");
        apply(v, a, R"({"sid":"s","seq":8,"ts":0,"ev":"mode","mode":"default","by":"approved",)"
                    R"("path":"/s/plan.md"})");
        expect(t, "view.modeoff", concat(&a, istr(&a, v->plan), concat(&a, " ", v->planpath)),
               "0 /s/plan.md");
        // Bypass mode, then Build again
        apply(v, a, R"({"sid":"s","seq":9,"ts":0,"ev":"mode","mode":"bypass","by":"user",)"
                    R"("path":"/s/plan.md"})");
        expect(t, "view.bypass", concat(&a, istr(&a, v->plan), istr(&a, v->bypass)), "01");
        apply(v, a, R"({"sid":"s","seq":10,"ts":0,"ev":"mode","mode":"default","by":"user",)"
                    R"("path":"/s/plan.md"})");
        expect(t, "view.bypassoff", concat(&a, istr(&a, v->plan), istr(&a, v->bypass)), "00");
    }

    // A desktop ask offers its class for good, and one for run offers
    // nothing; a desktop grant record changes nothing shown
    {
        Arena a = perm;
        View *v = testview(&a);
        apply(v, a, R"({"sid":"s","seq":1,"ts":0,"ev":"session","cwd":"/w"})");
        apply(v, a, R"({"sid":"s","seq":2,"ts":0,"ev":"assistant","turn":1,"attempt":1,"blocks":[)"
                    R"({"t":"call","id":"d1","name":"desktop","args":"{\"action\":\"type\"}"},)"
                    R"({"t":"call","id":"d2","name":"desktop","args":"{\"action\":\"run\"}"}]})");
        apply(v, a, R"({"sid":"s","seq":3,"ts":0,"ev":"ask","req":"d1/ask","call":"d1",)"
                    R"("kind":"desktop","tool":"desktop","detail":"desktop: type \"hi\"",)"
                    R"("always":"input","scope":"desktop","deadline":30000,"dflt":"deny"})");
        ViewTool *k = v->msgs[2].tool;
        expect(t, "view.deskalways", concat(&a, k->grant, concat(&a, " ", k->scope)),
               "input desktop");
        expect(t, "view.deskaccess", istr(&a, k->access.len + k->path.len), "0");
        apply(v, a, R"({"sid":"s","seq":4,"ts":0,"ev":"answer","req":"d1/ask","decision":"allow",)"
                    R"("by":"user","note":"","always":1})");
        apply(v, a, R"({"sid":"s","seq":5,"ts":0,"ev":"grant","desktop":"input","by":"user",)"
                    R"("req":"d1/ask"})");
        expect(t, "view.deskanswer", istr(&a, v->asks*100 + k->answer*10 + k->always),
               concat(&a, istr(&a, VASK_ALLOWED), "1"));
        apply(v, a, R"({"sid":"s","seq":6,"ts":0,"ev":"ask","req":"d2/ask","call":"d2",)"
                    R"("kind":"desktop","tool":"desktop","detail":"desktop: run x",)"
                    R"("deadline":30000,"dflt":"deny"})");
        ViewTool *r = v->msgs[3].tool;
        expect(t, "view.deskrun", istr(&a, r->grant.len + r->scope.len + v->asks*100), "100");
        expect(t, "view.deskseq", istr(&a, v->lastseq), "6");
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

    // The engine stops mid-turn, with no records to end it: a command
    // running, an ask open, text streaming. Nothing counts as running
    // or waiting any more.
    {
        Arena a = perm;
        View *v = testview(&a);
        apply(v, a, R"({"sid":"s","seq":1,"ts":0,"ev":"user","text":"go","via":"prompt"})");
        apply(v, a, R"({"sid":"s","seq":2,"ts":0,"ev":"turn","n":1})");
        apply(v, a, R"({"sid":"s","seq":3,"ts":0,"ev":"assistant","turn":1,"attempt":1,"blocks":[)"
                    R"({"t":"call","id":"c1","name":"shell","args":"{\"command\":\"sleep 60\"}"},)"
                    R"({"t":"call","id":"c2","name":"write","args":"{\"path\":\"../y\",\"content\":\"y\"}"}]})");
        apply(v, a, R"({"sid":"s","seq":4,"ts":0,"ev":"tool.start","call":"c1","name":"shell","args":"{}","cwd":"/w"})");
        apply(v, a, R"({"sid":"s","seq":5,"ts":0,"ev":"ask","req":"c2/ask","call":"c2","kind":"path",)"
                    R"("tool":"write","path":"/x/y","detail":"outside","deadline":30000,"dflt":"deny"})");
        apply(v, a, R"({"sid":"s","ts":0,"ev":"live","state":"tool"})");
        apply(v, a, R"({"sid":"s","ts":0,"ev":"delta","turn":1,"attempt":2,"kind":"text","text":"so"})");
        expect(t, "view.running2", istr(&a, v->busy*10 + v->asks), "11");
        viewstopped(v);
        expect(t, "view.stopped", msgsshow(&a, v),
               "U:go;X[shell sleep 60 unknown]:;X[write ../y unknown]:;K:outside;A~:so;");
        ViewTool *k = v->msgs[3].tool;
        expect(t, "view.stoppedask", istr(&a, k->answer*10 + (k->by=="stopped")), "21");
        expect(t, "view.stoppedidle", istr(&a, v->busy*100 + v->asks*10 + (v->state==LIVE_CLOSED)), "1");
        expect(t, "view.stoppedfade", istr(&a, v->msgs[4].faded*10 + (v->stream<0)), "11");
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

// The M4 records: the panels' todo list, jobs, snapshots and rewinds,
// instruction files; cards for compactions and rewinds; notices; turns
// a session began by itself; the liveness of queues, subagents and
// fallbacks.
static void test_viewpanels(Test *t)
{
    Arena perm = t->perm;

    // A subagent's session record, and its todo list replaced whole,
    // its row folded with counts, the list as a string of JSON too
    {
        Arena a = perm;
        View *v = testview(&a);
        apply(v, a, R"({"sid":"k","seq":1,"ts":0,"ev":"session","cwd":"/w","role":"explore",)"
                    R"("model":"m","endpoint":"u","parent":"p","call":"c7","depth":1,)"
                    R"("tools":["read","grep"],"prompt":"Look."})");
        expect(t, "panels.parent", concat(&a, concat(&a, v->parent, " "), v->pcall), "p c7");
        expect(t, "panels.tools", v->tools, "read grep");
        expect(t, "panels.depth", istr(&a, v->depth), "1");
        apply(v, a, R"({"sid":"k","seq":2,"ts":0,"ev":"assistant","turn":1,"attempt":1,)"
                    R"("blocks":[{"t":"call","id":"c1","name":"todo_write","args":)"
                    R"("{\"todos\":\"[{\\\"content\\\":\\\"a\\\",\\\"status\\\":\\\"done\\\"},)"
                    R"({\\\"content\\\":\\\"b\\\",\\\"status\\\":\\\"in_progress\\\"}]\"}"}]})");
        expect(t, "panels.todorow", msgsshow(&a, v),
               "X[todo_write 2 items, 1 done, 1 in progress called]:;");
        expect(t, "panels.todofolded", istr(&a, v->msgs[0].collapsed), "1");
        apply(v, a, R"({"sid":"k","seq":3,"ts":0,"ev":"todo","call":"c1","items":[)"
                    R"({"content":"a","status":"done"},{"content":"b","status":"in_progress"},)"
                    R"({"content":"c","status":"pending"}]})");
        Buf b(&a, 64);
        for (iz i = 0; i < v->todos.len; i++) {
            print(&b, v->todos[i].content);
            print(&b, (i64)v->todos[i].status);
        }
        expect(t, "panels.todo", finish(&b), "a2b1c0");
        apply(v, a, R"({"sid":"k","seq":4,"ts":0,"ev":"todo","call":"c2","items":[]})");
        expect(t, "panels.todoempty", istr(&a, v->todos.len), "0");
    }

    // Jobs: started, their output, ended; the row names its job, and
    // a stopped engine leaves none running
    {
        Arena a = perm;
        View *v = testview(&a);
        apply(v, a, R"({"sid":"s","seq":1,"ts":0,"ev":"assistant","turn":1,"attempt":1,)"
                    R"("blocks":[{"t":"call","id":"c1","name":"shell","args":"{\"command\":\"x\"}"}]})");
        apply(v, a, R"({"sid":"s","seq":2,"ts":0,"ev":"tool.start","call":"c1","name":"shell","args":"{}"})");
        apply(v, a, R"({"sid":"s","seq":3,"ts":1000,"ev":"job.start","job":1,"call":"c1",)"
                    R"("command":"make\nmore","spill":"out/c1.log","deadline":9000})");
        apply(v, a, R"({"sid":"s","seq":4,"ts":0,"ev":"tool.end","call":"c1","status":"ok",)"
                    R"("text":"Started background job 1.","job":1,"ms":3})");
        apply(v, a, R"({"sid":"s","seq":5,"ts":0,"ev":"job.start","job":2,"call":"c2",)"
                    R"("command":"b","spill":"out/c1b.log","deadline":9000})");
        b32 out = apply(v, a, R"({"sid":"s","ts":0,"ev":"job.out","job":1,"call":"c1","tail":"one\ntwo\n","bytes":8})");
        ViewJob *j = findjob(v, 1);
        expect(t, "panels.jobout", concat(&a, istr(&a, out), Str{j->tail, j->taillen}), "1one\ntwo\n");
        expect(t, "panels.jobsummary", j->summary, "make\xe2\x80\xa6");
        expect(t, "panels.jobrun", istr(&a, v->jobsrun), "2");
        expect(t, "panels.jobrow", istr(&a, v->msgs[0].tool->job*10 + v->msgs[0].collapsed), "11");
        apply(v, a, R"({"sid":"s","seq":6,"ts":0,"ev":"job.end","job":1,"call":"c1","status":"exited",)"
                    R"("exit":0,"bytes":8,"spill":"out/c1.log","ms":500,"notice":1,"resume":0})");
        out = apply(v, a, R"({"sid":"s","ts":0,"ev":"job.out","job":1,"call":"c1","tail":"late","bytes":9})");
        expect(t, "panels.jobended", concat(&a, istr(&a, out), vjobnames[j->status]), "0exited");
        expect(t, "panels.jobexit", istr(&a, j->hasexit*10 + j->exit + j->ms), "510");
        viewstopped(v);
        expect(t, "panels.jobstopped", vjobnames[findjob(v, 2)->status], "unknown");
        expect(t, "panels.jobnone", istr(&a, v->jobsrun), "0");
    }

    // Snapshots and rewinds: the files a rewind would put back, by call
    // and by turn, then a rewind's card, its conflicts, and the undone
    {
        Arena a = perm;
        View *v = testview(&a);
        apply(v, a, R"({"sid":"s","seq":1,"ts":0,"ev":"session","cwd":"/w","model":"m"})");
        apply(v, a, R"({"sid":"s","seq":2,"ts":0,"ev":"user","text":"go","via":"prompt"})");
        apply(v, a, R"({"sid":"s","seq":3,"ts":0,"ev":"turn","n":1})");
        expect(t, "panels.startsturn", istr(&a, v->msgs[0].startsturn), "1");
        char const *calls[] = {
            R"({"sid":"s","seq":4,"ts":0,"ev":"assistant","turn":1,"attempt":1,"blocks":[)"
            R"({"t":"call","id":"w1","name":"write","args":"{\"path\":\"/w/a\"}"},)"
            R"({"t":"call","id":"e1","name":"edit","args":"{\"path\":\"/w/a\"}"},)"
            R"({"t":"call","id":"w2","name":"write","args":"{\"path\":\"/w/b\"}"}]})",
            R"({"sid":"s","seq":5,"ts":0,"ev":"tool.start","call":"w1","name":"write","args":"{}"})",
            R"({"sid":"s","seq":6,"ts":0,"ev":"snap","call":"w1","path":"/w/a","snap":"snap/1","existed":0})",
            R"({"sid":"s","seq":7,"ts":0,"ev":"tool.end","call":"w1","status":"ok","text":"Created."})",
            R"({"sid":"s","seq":8,"ts":0,"ev":"tool.start","call":"e1","name":"edit","args":"{}"})",
            R"({"sid":"s","seq":9,"ts":0,"ev":"snap","call":"e1","path":"/w/a","snap":"snap/2","existed":1})",
            R"({"sid":"s","seq":10,"ts":0,"ev":"tool.end","call":"e1","status":"error","text":"x"})",
            R"({"sid":"s","seq":11,"ts":0,"ev":"tool.start","call":"w2","name":"write","args":"{}"})",
            R"({"sid":"s","seq":12,"ts":0,"ev":"snap","call":"w2","path":"/w/b","snap":"snap/3","existed":1})",
            R"({"sid":"s","seq":13,"ts":0,"ev":"tool.end","call":"w2","status":"ok","text":"Replaced."})",
            R"({"sid":"s","seq":14,"ts":0,"ev":"turn.end","turn":1,"why":"done"})",
        };
        for (iz i = 0; i < countof(calls); i++) {
            Str line = {(u8 *)calls[i], (iz)__builtin_strlen(calls[i])};
            apply(v, a, line);
        }
        Slice<ViewSnap> s = rewindset(v, &a, "e1", 0);
        Buf b(&a, 64);
        for (iz i = 0; i < s.len; i++) {
            print(&b, s[i].path);
            print(&b, (i64)s[i].existed);
        }
        expect(t, "panels.rewindcall", finish(&b), "/w/b1/w/a1");
        s = rewindset(v, &a, {}, 1);
        b.len = 0;
        for (iz i = 0; i < s.len; i++) {
            print(&b, s[i].path);
            print(&b, (i64)s[i].existed);
        }
        expect(t, "panels.rewindturn", finish(&b), "/w/b1/w/a0");
        expect(t, "panels.turnrewinds", istr(&a, turnrewinds(v, 1)*10 + turnrewinds(v, 2)), "10");
        apply(v, a, R"({"sid":"s","seq":15,"ts":0,"ev":"rewind","to":"w2","force":0,)"
                    R"("files":[{"path":"/w/b","result":"conflict"}],"undone":[],"note":"n"})");
        ViewMsg *c = &v->msgs[v->msgs.len-1];
        expect(t, "panels.rewindcard", concat(&a, c->card->to, istr(&a, c->card->conflicts)), "w21");
        expect(t, "panels.rewindtext", c->text,
               "- `b`: **conflict**, left as it is: something else changed it after "
               "the harness did\n\n*n*\n");
        v->foot = 10;
        expect(t, "panels.rewindfoot", istr(&a, (i64)msgfoot(v, c)), "10");
        apply(v, a, R"({"sid":"s","seq":16,"ts":0,"ev":"rewind","to":1,"force":1,)"
                    R"("files":[{"path":"/w/b","result":"restored"},{"path":"/w/a","result":"removed"}],)"
                    R"("undone":["snap/3","snap/2","snap/1"],"note":""})");
        c = &v->msgs[v->msgs.len-1];
        expect(t, "panels.forced", istr(&a, c->card->turn*100 + c->card->count*10 + c->card->force), "121");
        expect(t, "panels.oldcard", istr(&a, (i64)msgfoot(v, &v->msgs[v->msgs.len-2])), "0");
        expect(t, "panels.undone", istr(&a, rowsnap(v, v->msgs[1].tool)->undone), "1");
        expect(t, "panels.nothing", istr(&a, rewindset(v, &a, {}, 1).len + turnrewinds(v, 1)), "0");
        // Put back in part: a later change undone, an earlier one kept
        // for a change made in between, which forcing would undo
        apply(v, a, R"({"sid":"s","seq":17,"ts":0,"ev":"rewind","to":1,"force":0,)"
                    R"("files":[{"path":"/w/b","result":"partial"}],"undone":[],"note":""})");
        c = &v->msgs[v->msgs.len-1];
        expect(t, "panels.partial", c->text, "- `b`: put back **in part**, to before the "
               "later changes: something else changed it in between, so the earlier "
               "ones stay\n");
        expect(t, "panels.partialforce", istr(&a, c->card->conflicts), "1");
    }

    // Compactions as cards: a pruning that marks the rows it elided, and
    // a summary of the messages before its cut
    {
        Arena a = perm;
        View *v = testview(&a);
        apply(v, a, R"({"sid":"s","seq":1,"ts":0,"ev":"user","text":"go"})");
        apply(v, a, R"({"sid":"s","seq":2,"ts":0,"ev":"assistant","turn":1,"attempt":1,)"
                    R"("blocks":[{"t":"call","id":"r1","name":"read","args":"{\"path\":\"f\"}"}]})");
        apply(v, a, R"({"sid":"s","seq":3,"ts":0,"ev":"tool.start","call":"r1","name":"read","args":"{}"})");
        apply(v, a, R"({"sid":"s","seq":4,"ts":0,"ev":"tool.end","call":"r1","status":"ok","text":"x"})");
        apply(v, a, R"({"sid":"s","seq":5,"ts":0,"ev":"compact","turn":1,"kind":"prune","why":"threshold",)"
                    R"("before":900,"after":400,"elided":[{"seq":4,"text":"[output elided: 9 tokens]"}]})");
        ViewMsg *c = &v->msgs[v->msgs.len-1];
        expect(t, "panels.prune", c->text, "- **read** `f`: `[output elided: 9 tokens]`\n");
        expect(t, "panels.elided", istr(&a, v->msgs[1].tool->elided + c->collapsed*10 + c->fold*100), "111");
        apply(v, a, R"({"sid":"s","seq":6,"ts":0,"ev":"compact","turn":1,"kind":"summary","why":"overflow",)"
                    R"("before":500,"after":100,"cutseq":5,"summary":"## Goal\nGo.","model":"m","ms":7})");
        c = &v->msgs[v->msgs.len-1];
        expect(t, "panels.summary", concat(&a, c->text, istr(&a, c->card->count)), "## Goal\nGo.2");
        expect(t, "panels.cut", concat(&a, c->card->why, istr(&a, v->cutseq)), "overflow5");
    }

    // A compaction the user asked for: its turn starts at no message,
    // the prompt before keeps its own turn, and the card says manual; a
    // model switch, and titles by whoever set them
    {
        Arena a = perm;
        View *v = testview(&a);
        apply(v, a, R"({"sid":"s","seq":1,"ts":0,"ev":"session","cwd":"/w","model":"m","endpoint":"http://a/"})");
        apply(v, a, R"({"sid":"s","seq":2,"ts":0,"ev":"user","text":"go"})");
        apply(v, a, R"({"sid":"s","seq":3,"ts":0,"ev":"title","text":"go","by":"auto"})");
        apply(v, a, R"({"sid":"s","seq":4,"ts":0,"ev":"turn","n":1})");
        apply(v, a, R"({"sid":"s","seq":5,"ts":0,"ev":"turn.end","turn":1,"why":"done",)"
                    R"("ctx":1000,"compact_at":850,"tokens":12})");
        apply(v, a, R"({"sid":"s","seq":6,"ts":0,"ev":"user","text":"queued, then cancelled"})");
        apply(v, a, R"({"sid":"s","seq":7,"ts":0,"ev":"turn","n":2,"via":"compact"})");
        expect(t, "panels.compactturn", concat(&a, istr(&a, v->msgs[0].startsturn),
               istr(&a, v->msgs[1].startsturn)), "10");
        apply(v, a, R"({"sid":"s","seq":8,"ts":0,"ev":"compact","turn":2,"kind":"summary","why":"manual",)"
                    R"("before":500,"after":100,"cutseq":6,"summary":"S","model":"m","ms":7,"focus":"f"})");
        apply(v, a, R"({"sid":"s","seq":9,"ts":0,"ev":"turn.end","turn":2,"why":"done"})");
        ViewMsg *c = &v->msgs[v->msgs.len-1];
        expect(t, "panels.manual", concat(&a, c->card->why, istr(&a, v->busy)), "manual0");
        apply(v, a, R"({"sid":"s","seq":10,"ts":0,"ev":"model","endpoint":"http://b/","model":"n","ctx":9})");
        apply(v, a, R"({"sid":"s","seq":11,"ts":0,"ev":"title","text":"Mine","by":"user"})");
        expect(t, "panels.switched", concat(&a, concat(&a, v->model, v->endpoint), v->title),
               "nhttp://b/Mine");

        // Each change a mark: a mode only as it changes, a title only
        // the user's
        apply(v, a, R"({"sid":"s","seq":12,"ts":0,"ev":"mode","mode":"plan","by":"user"})");
        apply(v, a, R"({"sid":"s","seq":13,"ts":0,"ev":"mode","mode":"plan","by":"agent"})");
        apply(v, a, R"({"sid":"s","seq":14,"ts":0,"ev":"title","text":"Auto","by":"auto"})");
        apply(v, a, R"({"sid":"s","seq":15,"ts":0,"ev":"mode","mode":"none","by":"approved"})");
        Buf marks(&a, 256);
        for (iz i = 0; i < v->msgs.len; i++) {
            ViewMsg *m = &v->msgs[i];
            if (m->role == VIEW_MARK) {
                print(&marks, m->label);
                putbyte(&marks, '=');
                print(&marks, m->text);
                print(&marks, m->from.len ? concat(&a, "@", m->from) : Str{});
                print(&marks, istr(&a, m->collapsed));
                putbyte(&marks, '|');
            }
        }
        expect(t, "panels.marks", finish(&marks),
               "Model=n@http://b/1|Renamed=Mine1|"
               "Plan mode=the agent changes nothing but its plan1|"
               "Build mode=plan mode and Bypass are off1|");
    }

    // A steer too late for its turn stays queued through the turn's end,
    // as the engine's next record says it is a prompt now, and leaves as
    // that prompt is taken in; a stop empties the queue
    {
        Arena a = perm;
        View *v = testview(&a);
        apply(v, a, R"({"sid":"s","seq":1,"ts":0,"ev":"session","cwd":"/w","model":"m"})");
        apply(v, a, R"({"sid":"s","seq":2,"ts":0,"ev":"user","text":"go"})");
        apply(v, a, R"({"sid":"s","seq":3,"ts":0,"ev":"turn","n":1})");
        apply(v, a, R"({"sid":"s","ts":0,"ev":"queue","items":[{"id":1,"kind":"steer","text":"late"}]})");
        apply(v, a, R"({"sid":"s","seq":4,"ts":0,"ev":"turn.end","turn":1,"why":"done"})");
        expect(t, "panels.queuekept", istr(&a, v->queue.len*10 + v->queue[0].steer), "11");
        apply(v, a, R"({"sid":"s","seq":5,"ts":0,"ev":"user","text":"late","via":"queue"})");
        expect(t, "panels.queuetaking", istr(&a, v->queue.len), "1");
        apply(v, a, R"({"sid":"s","ts":0,"ev":"queue","items":[]})");
        expect(t, "panels.queuetaken", istr(&a, v->queue.len), "0");
        apply(v, a, R"({"sid":"s","ts":0,"ev":"queue","items":[{"id":2,"kind":"prompt","text":"x"}]})");
        viewstopped(v);
        expect(t, "panels.queuestopped", istr(&a, v->queue.len), "0");
    }

    // Where a fork at each message cuts (viewforkat), by the engine's turn
    // numbers: turns begun by a job's end and by a manual compaction count,
    // a steer is no prompt of its own, and a queued prompt is one; the
    // records as the engine logs them, a title between a prompt and its turn
    {
        Arena a = perm;
        View *v = testview(&a);
        Str log[] = {
            R"({"sid":"s","seq":1,"ts":0,"ev":"session","cwd":"/w","model":"m"})",
            R"({"sid":"s","seq":2,"ts":0,"ev":"user","text":"one","via":"prompt"})",
            R"({"sid":"s","seq":3,"ts":0,"ev":"title","text":"one","by":"auto"})",
            R"({"sid":"s","seq":4,"ts":0,"ev":"turn","n":1})",
            R"({"sid":"s","seq":5,"ts":0,"ev":"user","text":"wait","via":"steer"})",
            R"({"sid":"s","seq":6,"ts":0,"ev":"assistant","turn":1,"attempt":1,)"
                R"("blocks":[{"t":"text","text":"A1"}]})",
            R"({"sid":"s","seq":7,"ts":0,"ev":"turn.end","turn":1,"why":"done"})",
            R"({"sid":"s","seq":8,"ts":0,"ev":"turn","n":2,"via":"job"})",
            R"({"sid":"s","seq":9,"ts":0,"ev":"assistant","turn":2,"attempt":1,)"
                R"("blocks":[{"t":"text","text":"A2"}]})",
            R"({"sid":"s","seq":10,"ts":0,"ev":"turn.end","turn":2,"why":"done"})",
            R"({"sid":"s","seq":11,"ts":0,"ev":"turn","n":3,"via":"compact"})",
            R"({"sid":"s","seq":12,"ts":0,"ev":"compact","turn":3,"kind":"summary","why":"manual",)"
                R"("before":500,"after":100,"cutseq":6,"summary":"S","model":"m","ms":7})",
            R"({"sid":"s","seq":13,"ts":0,"ev":"turn.end","turn":3,"why":"done"})",
            R"({"sid":"s","seq":14,"ts":0,"ev":"user","text":"two","via":"prompt"})",
            R"({"sid":"s","seq":15,"ts":0,"ev":"turn","n":4})",
            R"({"sid":"s","seq":16,"ts":0,"ev":"assistant","turn":4,"attempt":1,)"
                R"("blocks":[{"t":"text","text":"A4"}]})",
            R"({"sid":"s","seq":17,"ts":0,"ev":"turn.end","turn":4,"why":"done"})",
            R"({"sid":"s","seq":18,"ts":0,"ev":"user","text":"three","via":"queue"})",
            R"({"sid":"s","seq":19,"ts":0,"ev":"turn","n":5})",
            R"({"sid":"s","seq":20,"ts":0,"ev":"assistant","turn":5,"attempt":1,)"
                R"("blocks":[{"t":"text","text":"A5"}]})",
            R"({"sid":"s","seq":21,"ts":0,"ev":"turn.end","turn":5,"why":"done"})",
        };
        for (iz i = 0; i < countof(log); i++) {
            apply(v, a, log[i]);
        }
        Buf b(&a, 256);
        for (iz i = 0; i < v->msgs.len; i++) {
            ViewMsg *m      = &v->msgs[i];
            b32      before = 0;
            i64      at     = viewforkat(m, &before);
            if (m->role==VIEW_USER || m->role==VIEW_ASSISTANT || m->role==VIEW_NOTE) {
                print(&b, m->role==VIEW_NOTE ? Str("note") : m->text);
                putbyte(&b, before ? '<' : '>');
                print(&b, at);
                putbyte(&b, ' ');
            }
        }
        expect(t, "panels.forkat", finish(&b),
               "one<0 wait>1 A1>1 note>2 A2>2 two<3 A4>4 three<4 A5>5 ");
    }

    // A turn the session began by itself, its notice folded; liveness
    {
        Arena a = perm;
        View *v = testview(&a);
        apply(v, a, R"({"sid":"s","seq":1,"ts":0,"ev":"session","cwd":"/w","model":"m"})");
        apply(v, a, R"({"sid":"s","seq":2,"ts":0,"ev":"turn","n":1,"via":"job"})");
        apply(v, a, R"({"sid":"s","seq":3,"ts":0,"ev":"notice","kind":"job","job":3,"text":"Job 3 ended."})");
        expect(t, "panels.resumed", msgsshow(&a, v),
               "N:A background job ended, so the agent went on without a prompt.;N:Job 3 ended.;");
        expect(t, "panels.resumeturn", istr(&a, v->msgs[0].startsturn), "1");
        expect(t, "panels.notice", concat(&a, v->msgs[1].label, istr(&a, v->msgs[1].collapsed)),
               "To the agent: job 3 ended1");
        apply(v, a, R"({"sid":"s","seq":4,"ts":0,"ev":"user","text":"wait","via":"steer"})");
        apply(v, a, R"({"sid":"s","seq":5,"ts":0,"ev":"turn","n":2})");
        expect(t, "panels.steerturn", istr(&a, v->msgs[2].startsturn), "0");
        b32 ch = apply(v, a, R"({"sid":"s","ts":0,"ev":"live","state":"queued","queue_pos":2,"queue_len":3})");
        expect(t, "panels.queued", istr(&a, ch*1000 + v->state*100 + v->queuepos*10 + v->queuelen),
               istr(&a, 1000 + LIVE_QUEUED*100 + 23));
        ch = apply(v, a, R"({"sid":"s","ts":0,"ev":"live","state":"queued","queue_pos":1,"queue_len":3})");
        expect(t, "panels.queuemoved", istr(&a, ch), "1");
        apply(v, a, R"({"sid":"s","ts":0,"ev":"queue","items":[)"
                    R"({"id":4,"kind":"steer","text":"look","cut":9,"images":2},)"
                    R"({"id":5,"kind":"prompt","text":"then this"}]})");
        {
            Buf b(&a, 128);
            for (iz i = 0; i < v->queue.len; i++) {
                ViewQueued *q = &v->queue[i];
                print(&b, (i64)q->id);
                print(&b, q->steer ? Str("s") : Str("p"));
                print(&b, (i64)q->cut);
                print(&b, (i64)q->images);
                print(&b, q->text);
                print(&b, Str(";"));
            }
            expect(t, "panels.queue", finish(&b), "4s12look;5p00then this;");
        }
        ch = apply(v, a, R"({"sid":"s","ts":0,"ev":"live","state":"task","tasks":2})");
        expect(t, "panels.tasks", istr(&a, ch*100 + v->state*10 + v->tasks), istr(&a, 100 + LIVE_TASK*10 + 2));
        expect(t, "panels.compacting", istr(&a, livestate("compacting")), istr(&a, LIVE_COMPACTING));
        apply(v, a, R"({"sid":"s","seq":6,"ts":0,"ev":"retry","turn":2,"attempt":1,"class":"limit",)"
                    R"("status":429,"delay_ms":0,"msg":"quota","endpoint":"u","model":"fb","fallback":1})");
        expect(t, "panels.fallback", concat(&a, v->retrymodel, istr(&a, v->fallback)), "fb1");
        expect(t, "panels.fallbacknote", v->msgs[v->msgs.len-1].text,
               "Attempt 1 failed (limit, HTTP 429): quota. Switching to fallback 1, fb.");
        apply(v, a, R"({"sid":"s","seq":7,"ts":0,"ev":"retry","turn":2,"attempt":2,"class":"transient",)"
                    R"("status":503,"delay_ms":90000,"msg":"busy","endpoint":"u","model":"fb","slow":1})");
        expect(t, "panels.slow", v->msgs[v->msgs.len-1].text,
               "Attempt 2 failed (transient, HTTP 503): busy. Waiting 1 min 30 s before "
               "trying again (a slow retry), with fb.");
        expect(t, "panels.stillfb", istr(&a, v->fallback*10 + v->retryslow), "11");
        apply(v, a, R"({"sid":"s","seq":8,"ts":0,"ev":"retry","turn":2,"attempt":3,"class":"transient",)"
                    R"("status":503,"delay_ms":500,"msg":"busy","endpoint":"u","model":"m"})");
        expect(t, "panels.primary", istr(&a, v->fallback), "0");
    }

    // A skill the user invoked: /name and its args, the body folded
    {
        Arena a = perm;
        View *v = testview(&a);
        apply(v, a, R"({"sid":"s","seq":1,"ts":0,"ev":"user","via":"prompt","skill":"pdf",)"
                    R"("args":"x.pdf","text":"<skill name=\"pdf\" dir=\"/s/pdf\">\nRead it.\n</skill>\n\nx.pdf"})");
        expect(t, "panels.skill", msgsshow(&a, v),
               "U:/pdf x.pdf;N:<skill name=\"pdf\" dir=\"/s/pdf\">\nRead it.\n</skill>;");
        expect(t, "panels.skillnote", concat(&a, v->msgs[1].label, istr(&a, v->msgs[1].collapsed)),
               "To the agent: skill pdf1");
    }

    // Instruction files, each once as last told; a task's report as
    // markdown, its subagent named; a command moved to the background
    {
        Arena a = perm;
        View *v = testview(&a);
        apply(v, a, R"({"sid":"s","seq":1,"ts":0,"ev":"instructions","why":"start","files":[)"
                    R"({"path":"/w/AGENTS.md","bytes":10,"mtime":1},)"
                    R"({"path":"/w/x.md","bytes":5,"mtime":1,"importer":"/w/AGENTS.md"}],"text":"T"})");
        apply(v, a, R"({"sid":"s","seq":2,"ts":0,"ev":"instructions","why":"changed","files":[)"
                    R"({"path":"/w/AGENTS.md","bytes":12,"mtime":2}],"skipped":["/w/y.md"]})");
        Buf b(&a, 128);
        for (iz i = 0; i < v->instr.len; i++) {
            print(&b, v->instr[i].path);
            putbyte(&b, ' ');
            print(&b, v->instr[i].why);
            putbyte(&b, ' ');
            print(&b, v->instr[i].bytes);
            putbyte(&b, ';');
        }
        expect(t, "panels.instr", finish(&b), "/w/AGENTS.md changed 12;/w/x.md start 5;");
        expect(t, "panels.instrtext", concat(&a, v->instrtext, v->skipped[0]), "T/w/y.md");

        // Memory: the index's memories as last told, hand-written lines not
        View *mv = testview(&a);
        apply(mv, a, R"({"sid":"s","seq":1,"ts":0,"ev":"memory","why":"start","dir":"/m/-w",)"
                     R"("text":"# Mine\n- [hosts](hosts.md) — Test hosts\n"})");
        apply(mv, a, R"({"sid":"s","seq":2,"ts":0,"ev":"memory","why":"saved","dir":"/m/-w",)"
                     R"("text":"# Mine\n- [hosts](hosts.md) — Test hosts\n- [b](b.md): B\n"})");
        apply(mv, a, R"({"sid":"s","seq":3,"ts":0,"ev":"notice","kind":"memory","text":"Changed"})");
        b.len = 0;
        for (iz i = 0; i < mv->mems.len; i++) {
            print(&b, mv->mems[i].name);
            putbyte(&b, '=');
            print(&b, mv->mems[i].desc);
            putbyte(&b, ';');
        }
        expect(t, "panels.mems", concat(&a, mv->memdir, finish(&b)), "/m/-whosts=Test hosts;b=B;");
        expect(t, "panels.memnotice", mv->msgs.len ? mv->msgs[mv->msgs.len-1].label : Str{},
               "To the agent: memory");
        Arena tmp = carve(&a, 1<<16);
        expect(t, "panels.memsave", toolsummary(&a, tmp, "memory",
               "{\"action\":\"save\",\"name\":\"hosts\"}", "/w"), "Save hosts");
        expect(t, "panels.memforget", toolsummary(&a, tmp, "memory",
               "{\"action\":\"forget\",\"name\":\"hosts\"}", "/w"), "Forget hosts");
        apply(v, a, R"({"sid":"s","seq":3,"ts":0,"ev":"assistant","turn":1,"attempt":1,"blocks":[)"
                    R"({"t":"call","id":"t1","name":"task","args":"{\"description\":\"Look\"}"},)"
                    R"({"t":"call","id":"s1","name":"shell","args":"{\"command\":\"sleep 9\"}"}]})");
        apply(v, a, R"({"sid":"s","seq":4,"ts":0,"ev":"tool.end","call":"t1","status":"ok",)"
                    R"("text":"**Found** it.","child":"kid","ms":5})");
        apply(v, a, R"({"sid":"s","seq":5,"ts":0,"ev":"tool.end","call":"s1","status":"background",)"
                    R"("text":"so far","job":4,"ms":5})");
        expect(t, "panels.task", msgsshow(&a, v),
               "X[task Look ok]:**Found** it.;X[shell sleep 9 background]:so far;");
        expect(t, "panels.taskmd", concat(&a, v->msgs[0].tool->child, istr(&a, v->msgs[0].md)), "kid1");
        expect(t, "panels.background", istr(&a, v->msgs[1].tool->job*10 + v->msgs[1].collapsed), "40");
    }

    // A subagent on another host: its host from the call's arguments,
    // its sid from task.host, kept by a tool.end that names none; and a
    // report from the background names its host
    {
        Arena a = perm;
        View *v = testview(&a);
        apply(v, a, R"({"sid":"s","seq":1,"ts":0,"ev":"assistant","turn":1,"attempt":1,"blocks":[)"
                    R"({"t":"call","id":"t1","name":"task","args":"{\"description\":\"Build\",\"host\":\"wsl\"}"},)"
                    R"({"t":"call","id":"t2","name":"task","args":"{\"description\":\"Here\"}"}]})");
        apply(v, a, R"({"sid":"s","seq":2,"ts":0,"ev":"tool.start","call":"t1","name":"task",)"
                    R"("args":"{\"description\":\"Build\",\"host\":\"wsl\"}"})");
        apply(v, a, R"({"sid":"s","seq":3,"ts":0,"ev":"tool.start","call":"t2","name":"task",)"
                    R"("args":"{\"description\":\"Here\"}"})");
        ViewTool *t1 = v->msgs[0].tool;
        expect(t, "panels.farhost", concat(&a, t1->host, istr(&a, v->hostcalls)), "wsl1");
        expect(t, "panels.farunnamed", concat(&a, t1->child, "|"), "|");
        apply(v, a, R"({"sid":"s","seq":4,"ts":0,"ev":"task.host","call":"t1","host":"wsl","child":"far1"})");
        expect(t, "panels.farnamed", concat(&a, t1->child, istr(&a, v->hostcalls)), "far11");
        apply(v, a, R"({"sid":"s","seq":5,"ts":0,"ev":"tool.end","call":"t1","status":"error",)"
                    R"("text":"Lost.","host":"wsl","ms":5})");
        expect(t, "panels.farend", concat(&a, t1->child, t1->host), "far1wsl");
        expect(t, "panels.nothost", concat(&a, v->msgs[1].tool->host, "|"), "|");
        apply(v, a, R"({"sid":"s","seq":6,"ts":0,"ev":"notice","kind":"task","child":"far2",)"
                    R"("host":"pi","text":"The subagent finished."})");
        ViewMsg *m = &v->msgs[v->msgs.len-1];
        expect(t, "panels.farreport", concat(&a, m->from, m->fromhost), "far2pi");

        // The child's own view: its parent on another host
        View *k = testview(&a);
        apply(k, a, R"({"sid":"far1","seq":1,"ts":0,"ev":"session","cwd":"/w","model":"m",)"
                    R"("parent":"s","call":"t1","depth":1,"remote":1,"via":"wsl","distro":"Ubuntu"})");
        expect(t, "panels.farchild", concat(&a, k->parent, istr(&a, k->remote)), "s1");
    }

    // Messages between sessions: the sender's row names the target as
    // given, then by its title; a message read shows open, its sender
    // named, and linked by sid
    {
        Arena a = perm;
        View *v = testview(&a);
        apply(v, a, R"({"sid":"s","seq":1,"ts":0,"ev":"assistant","turn":1,"attempt":1,"blocks":[)"
                    R"({"t":"call","id":"m1","name":"send_message","args":"{\"to\":\"k1\",\"text\":\"hi\"}"},)"
                    R"({"t":"call","id":"m2","name":"send_message","args":"{\"to\":\"nobody\"}"}]})");
        expect(t, "panels.msgcall", msgsshow(&a, v),
               "X[send_message Message \xe2\x86\x92 k1 called]:;"
               "X[send_message Message \xe2\x86\x92 nobody called]:;");
        apply(v, a, R"({"sid":"s","seq":2,"ts":0,"ev":"tool.end","call":"m1","status":"ok",)"
                    R"("text":"Sent.","to":"k1","to_title":"Fix the build","delivery":"woke",)"
                    R"("hop":1,"ms":1})");
        apply(v, a, R"({"sid":"s","seq":3,"ts":0,"ev":"tool.end","call":"m2","status":"error",)"
                    R"("text":"Not sent.","fail":"message","ms":1})");
        expect(t, "panels.msgsent", msgsshow(&a, v),
               "X[send_message Message \xe2\x86\x92 Fix the build ok]:Sent.;"
               "X[send_message Message \xe2\x86\x92 nobody error]:Not sent.;");
        expect(t, "panels.msgtarget", concat(&a, v->msgs[0].tool->child, "|"), "k1|");
        expect(t, "panels.msgnotarget", concat(&a, v->msgs[1].tool->child, "|"), "|");
        apply(v, a, R"({"sid":"s","seq":4,"ts":0,"ev":"notice","kind":"message","from":"k1",)"
                    R"("from_title":"Fix the build","hop":2,"text":"Message from another session"})");
        apply(v, a, R"({"sid":"s","seq":5,"ts":0,"ev":"notice","kind":"message",)"
                    R"("from_title":"","hop":1,"text":"Imported"})");
        ViewMsg *m = &v->msgs[2];
        expect(t, "panels.msgnote", concat(&a, concat(&a, m->label, "|"),
               concat(&a, m->from, istr(&a, m->fold*10 + m->collapsed))),
               "Message from Fix the build|k110");
        m = &v->msgs[3];
        expect(t, "panels.msgnofrom", concat(&a, concat(&a, m->label, "|"), m->from),
               "Message from another session|");
        // A subagent's report from the background, and the turn it began
        apply(v, a, R"({"sid":"s","seq":6,"ts":0,"ev":"turn","n":2,"via":"task"})");
        apply(v, a, R"({"sid":"s","seq":7,"ts":0,"ev":"notice","kind":"task","child":"k2",)"
                    R"("child_title":"Slow child","text":"The subagent finished."})");
        m = &v->msgs[4];
        expect(t, "panels.bgturn", concat(&a, m->label, istr(&a, m->startsturn)),
               "Resumed by itself2");
        m = &v->msgs[5];
        expect(t, "panels.bgreport", concat(&a, concat(&a, m->label, "|"),
               concat(&a, m->from, istr(&a, m->fromkid*10 + m->collapsed))),
               "To the agent: the report of Slow child|k211");
    }
}

// Reasoning as each message has it: f folded, o open or h hidden, then
// * if the user chose, ~ if provisional, its count and its seconds.
static Str thinkshow(Arena *a, View *v)
{
    Buf b(a, 128);
    for (iz i = 0; i < v->msgs.len; i++) {
        ViewMsg *m = &v->msgs[i];
        if (m->role != VIEW_THINK) {
            continue;
        }
        print(&b, msghidden(v, m) ? Str("h") : m->collapsed ? Str("f") : Str("o"));
        if (m->chosen) putbyte(&b, '*');
        if (m->prov)   putbyte(&b, '~');
        putbyte(&b, ' ');
        print(&b, m->chars);
        putbyte(&b, ' ');
        print(&b, (m->ts1 - m->ts0)/1000);
        putbyte(&b, ';');
    }
    return finish(&b);
}

static void test_viewthinking(Test *t)
{
    Arena perm = t->perm;
    Str   user = R"({"sid":"s","seq":1,"ts":0,"ev":"user","text":"hi"})";
    Str   turn = R"({"sid":"s","seq":2,"ts":0,"ev":"turn","n":1})";
    Str   att  = R"({"sid":"s","seq":3,"ts":0,"ev":"attempt","turn":1,"n":1})";
    Str   d1   = R"({"sid":"s","ts":1000,"ev":"delta","turn":1,"attempt":1,"kind":"think","text":"Hm, café"})";
    Str   d2   = R"({"sid":"s","ts":4500,"ev":"delta","turn":1,"attempt":1,"kind":"think","text":" ok"})";
    Str   d3   = R"({"sid":"s","ts":5000,"ev":"delta","turn":1,"attempt":1,"kind":"text","text":"Hi"})";
    Str   done = R"({"sid":"s","seq":4,"ts":6000,"ev":"assistant","turn":1,"attempt":1,)"
                 R"("blocks":[{"t":"think","text":"Hm, café ok"},{"t":"text","text":"Hi"}]})";

    // Each setting, streamed then committed: folded from the first delta
    // (fold), open throughout (open), or hidden (hide); counted in code
    // points, and timed from the first delta to the last
    Str want[][2] = {
        {"f~ 11 3;", "f 11 3;"},
        {"o~ 11 3;", "o 11 3;"},
        {"h~ 11 3;", "h 11 3;"},
    };
    for (i32 mode = 0; mode < THINK_COUNT; mode++) {
        Arena a = perm;
        View *v = testview(&a);
        viewthink(v, mode);
        apply(v, a, user);
        apply(v, a, turn);
        apply(v, a, att);
        apply(v, a, d1);
        apply(v, a, d2);
        apply(v, a, d3);
        Str name = concat(&a, "thinking.", thinknames[mode]);
        expect(t, concat(&a, name, ".streamed"), thinkshow(&a, v), want[mode][0]);
        apply(v, a, done);
        expect(t, concat(&a, name, ".committed"), thinkshow(&a, v), want[mode][1]);
        expect(t, concat(&a, name, ".msgs"), msgsshow(&a, v), "U:hi;T:Hm, caf\xc3\xa9 ok;A:Hi;");
    }

    // Opened by the user while it streams, it stays open through more
    // deltas, its commit, and settings that fold everything else
    {
        Arena a = perm;
        View *v = testview(&a);
        apply(v, a, user);
        apply(v, a, turn);
        apply(v, a, att);
        apply(v, a, d1);
        togglemsg(v, 1);
        apply(v, a, d2);
        expect(t, "thinking.opened", thinkshow(&a, v), "o*~ 11 3;");
        apply(v, a, d3);
        apply(v, a, done);
        expect(t, "thinking.keptopen", thinkshow(&a, v), "o* 11 3;");
        viewthink(v, THINK_HIDE);
        expect(t, "thinking.hidden", thinkshow(&a, v), "h* 11 3;");
        viewthink(v, THINK_FOLD);
        expect(t, "thinking.backopen", thinkshow(&a, v), "o* 11 3;");
        // Closed by the user under open, it stays closed
        viewthink(v, THINK_OPEN);
        togglemsg(v, 1);
        viewthink(v, THINK_FOLD);
        viewthink(v, THINK_OPEN);
        expect(t, "thinking.keptclosed", thinkshow(&a, v), "f* 11 3;");
    }

    // A response's reasoning in two parts, the second opened: the commit
    // gives each its own by their order, and a snapshot does too
    {
        Arena a = perm;
        View *v = testview(&a);
        apply(v, a, user);
        apply(v, a, turn);
        apply(v, a, att);
        apply(v, a, R"({"sid":"s","ts":1000,"ev":"delta","turn":1,"attempt":1,"kind":"think","text":"one"})");
        apply(v, a, R"({"sid":"s","ts":2000,"ev":"delta","turn":1,"attempt":1,"kind":"args","block":1,"id":"c1","name":"read","text":"{}"})");
        apply(v, a, R"({"sid":"s","ts":3000,"ev":"delta","turn":1,"attempt":1,"kind":"think","text":"two"})");
        togglemsg(v, 3);
        expect(t, "thinking.two", thinkshow(&a, v), "f~ 3 0;o*~ 3 0;");
        apply(v, a, R"({"sid":"s","ts":5000,"ev":"snapshot","turn":1,"attempt":1,"blocks":[)"
                    R"({"block":0,"kind":"think","text":"one"},{"block":1,"kind":"args","id":"c1","name":"read","text":"{}"},)"
                    R"({"block":2,"kind":"think","text":"two!"}]})");
        expect(t, "thinking.snapshot", thinkshow(&a, v), "f~ 3 0;o*~ 4 0;");
        apply(v, a, R"({"sid":"s","seq":4,"ts":6000,"ev":"assistant","turn":1,"attempt":1,"blocks":[)"
                    R"({"t":"think","text":"one"},{"t":"call","id":"c1","name":"read","args":"{}"},)"
                    R"({"t":"think","text":"two!"}]})");
        expect(t, "thinking.twocommitted", thinkshow(&a, v), "f 3 0;o* 4 0;");
    }

    // Hidden, reasoning takes no space and is never the message at a
    // height, even last; folded, it takes its header's. Copying across
    // it still takes its text.
    {
        Arena a = perm;
        View *v = testview(&a);
        v->extra = 10;
        v->gap   = 2;
        apply(v, a, user);
        apply(v, a, turn);
        apply(v, a, att);
        apply(v, a, d1);
        apply(v, a, d2);
        f64 folded = viewtops(v, 100, 10, 10);
        expect(t, "thinking.foldedh", istr(&a, (i64)folded), "32");
        viewthink(v, THINK_HIDE);
        f64 hidden = viewtops(v, 100, 10, 10);
        expect(t, "thinking.hiddenh", istr(&a, (i64)hidden), "20");
        expect(t, "thinking.findlast", istr(&a, viewfind(v, 25)), "0");
        apply(v, a, d3);
        viewtops(v, 100, 10, 10);
        expect(t, "thinking.findnext", istr(&a, viewfind(v, 22)), "2");
        expect(t, "thinking.findprev", istr(&a, viewfind(v, 5)), "0");
        selset(v, 0, 0, 2, 2);
        expect(t, "thinking.copy", copysel(v, &a, COPY_TEXT), "hi\n\nHm, caf\xc3\xa9 ok\n\nHi");
    }
}

// Tool rows and reasoning as each message has it: f folded or o open, h
// hidden, then * if the user chose.
static Str foldshow(Arena *a, View *v)
{
    Buf b(a, 128);
    for (iz i = 0; i < v->msgs.len; i++) {
        ViewMsg *m = &v->msgs[i];
        if (m->role!=VIEW_TOOL && m->role!=VIEW_THINK) {
            continue;
        }
        print(&b, msghidden(v, m) ? Str("h") : m->collapsed ? Str("f") : Str("o"));
        if (m->chosen) putbyte(&b, '*');
        putbyte(&b, ';');
    }
    return finish(&b);
}

// Hits as "msg:off" each.
static Str hitshow(Arena *a, ViewSearch *s)
{
    Buf b(a, 128);
    for (iz i = 0; i < s->hits.len; i++) {
        print(&b, (i64)s->hits[i].msg);
        putbyte(&b, ':');
        print(&b, (i64)s->hits[i].off);
        putbyte(&b, ';');
    }
    return finish(&b);
}

static ViewSearch *testsearch(Arena *a)
{
    Os *os = newos(a, 1<<12);
    iz  mb = (iz)1<<20;
    return newsearch(a, os_reserve(os, 16*mb), os_reserve(os, 16*mb), 256);
}

static void test_viewtranscript(Test *t)
{
    Arena perm = t->perm;
    Str   recs[] = {
        R"({"sid":"s","seq":1,"ts":0,"ev":"user","text":"go"})",
        R"({"sid":"s","seq":2,"ts":0,"ev":"turn","n":1})",
        R"({"sid":"s","seq":3,"ts":0,"ev":"attempt","turn":1,"n":1})",
        R"({"sid":"s","seq":4,"ts":0,"ev":"assistant","turn":1,"attempt":1,"blocks":[)"
        R"({"t":"think","text":"Needle in the reasoning"},)"
        R"({"t":"call","id":"c1","name":"read","args":"{\"path\":\"a\"}"},)"
        R"({"t":"call","id":"c2","name":"shell","args":"{\"command\":\"ls\"}"},)"
        R"({"t":"call","id":"c3","name":"shell","args":"{\"command\":\"bad\"}"}]})",
        R"({"sid":"s","seq":5,"ts":0,"ev":"tool.start","call":"c1","name":"read","args":"{}"})",
        R"({"sid":"s","seq":6,"ts":0,"ev":"tool.end","call":"c1","status":"ok","text":"a NEEDLE read"})",
        R"({"sid":"s","seq":7,"ts":0,"ev":"tool.start","call":"c2","name":"shell","args":"{}"})",
    };
    Str end2 = R"({"sid":"s","seq":8,"ts":0,"ev":"tool.end","call":"c2","status":"ok","text":)"
               R"("1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n12\n13\n14\n15\n16\n17\n18\n19\n20\n21\n22\n23\n24\n25\n26\n"})";
    Str end3 = R"({"sid":"s","seq":9,"ts":0,"ev":"tool.end","call":"c3","status":"error","text":"needle, failed"})";

    // Each mode: auto folds the read and the long shell output and opens
    // the failure; fold and open apply to every row; a row the user
    // opened or closed keeps it through its end and every mode
    {
        Str want[] = {"f;f;f;o;", "f;f;f;f;", "f;o;o;o;"};
        for (i32 mode = 0; mode < TOOLS_COUNT; mode++) {
            Arena a = perm;
            View *v = testview(&a);
            viewtools(v, mode);
            for (iz i = 0; i < countof(recs); i++) apply(v, a, recs[i]);
            apply(v, a, end2);
            apply(v, a, end3);
            expect(t, concat(&a, "tools.", toolsnames[mode]), foldshow(&a, v), want[mode]);
        }

        Arena a = perm;
        View *v = testview(&a);
        for (iz i = 0; i < countof(recs); i++) apply(v, a, recs[i]);
        expect(t, "tools.running", foldshow(&a, v), "f;f;o;o;");
        togglemsg(v, 3);  // folded by the user while it runs: tool.end leaves it
        apply(v, a, end2);
        togglemsg(v, 4);  // and the failure, before its end
        apply(v, a, end3);
        expect(t, "tools.chosen", foldshow(&a, v), "f;f;f*;f*;");
        viewtools(v, TOOLS_OPEN);
        expect(t, "tools.chosenopen", foldshow(&a, v), "f;o;f*;f*;");
        viewtools(v, TOOLS_AUTO);
        expect(t, "tools.back", foldshow(&a, v), "f;f;f*;f*;");

        // The session's toggles open everything of their kind, hidden
        // reasoning too, but for what the user chose, and off again fold
        // as before
        viewthink(v, THINK_HIDE);
        expect(t, "toggle.hidden", foldshow(&a, v), "h;f;f*;f*;");
        viewshowtools(v, 1);
        viewshowthink(v, 1);
        expect(t, "toggle.on", foldshow(&a, v), "o;o;f*;f*;");
        togglemsg(v, 2);  // closed by the user while all are open
        viewshowtools(v, 0);
        viewshowthink(v, 0);
        expect(t, "toggle.off", foldshow(&a, v), "h;f*;f*;f*;");
    }

    // A provisional call the user opened stays open once committed
    {
        Arena a = perm;
        View *v = testview(&a);
        apply(v, a, recs[0]);
        apply(v, a, recs[1]);
        apply(v, a, recs[2]);
        apply(v, a, R"({"sid":"s","ts":1,"ev":"delta","turn":1,"attempt":1,"kind":"args",)"
                    R"("block":0,"id":"c1","name":"read","text":"{}"})");
        togglemsg(v, 1);
        apply(v, a, R"({"sid":"s","seq":4,"ts":2,"ev":"assistant","turn":1,"attempt":1,"blocks":[)"
                    R"({"t":"call","id":"c1","name":"read","args":"{}"}]})");
        expect(t, "tools.provchosen", foldshow(&a, v), "o*;");
    }

    // Finding: ASCII case ignored, folded rows searched, hidden reasoning
    // not; the search keeps up with new text from where it changed, a
    // budget at a time
    {
        Arena       a = perm;
        View       *v = testview(&a);
        ViewSearch *s = testsearch(&a);
        for (iz i = 0; i < countof(recs); i++) apply(v, a, recs[i]);
        searchreset(s, v, "needle");
        expect(t, "find.done", istr(&a, searchstep(s, 1<<20)), "1");
        expect(t, "find.hits", hitshow(&a, s), "1:0;2:2;");
        apply(v, a, end2);
        apply(v, a, end3);
        searchstep(s, 1<<20);
        expect(t, "find.more", hitshow(&a, s), "1:0;2:2;4:0;");
        viewthink(v, THINK_HIDE);
        searchstep(s, 1<<20);
        expect(t, "find.hidden", hitshow(&a, s), "2:2;4:0;");
        viewthink(v, THINK_FOLD);
        searchreset(s, v, "NEEDLE");
        expect(t, "find.budget", istr(&a, searchstep(s, 10)), "0");
        expect(t, "find.partial", hitshow(&a, s), "1:0;");
        while (!searchstep(s, 10)) {}
        expect(t, "find.rest", hitshow(&a, s), "1:0;2:2;4:0;");
        expect(t, "find.from", istr(&a, searchfrom(s, 3, 0)), "2");
        // A folded hit opens as if chosen, and is not searched again
        s->cur = 1;
        searchreveal(s, &s->hits[1]);
        searchstep(s, 1<<20);
        expect(t, "find.revealed", foldshow(&a, v), "f;o*;f;o;");
        expect(t, "find.kept", istr(&a, s->cur), "1");
        searchreset(s, v, "");
        searchstep(s, 1<<20);
        expect(t, "find.empty", istr(&a, s->hits.len), "0");
    }

    // The context gauge: usage, then the engine's own numbers
    {
        Arena a = perm;
        View *v = testview(&a);
        apply(v, a, recs[0]);
        apply(v, a, recs[1]);
        apply(v, a, R"({"sid":"s","seq":3,"ts":0,"ev":"attempt","turn":1,"n":1,)"
                    R"("ctx":131072,"compact_at":104857,"tokens":9000})");
        apply(v, a, R"({"sid":"s","seq":4,"ts":0,"ev":"assistant","turn":1,"attempt":1,)"
                    R"("blocks":[{"t":"text","text":"hi"}],"usage":{"in":12000,"out":345,"cached":11000}})");
        Buf b(&a, 64);
        print(&b, v->ctx);
        putbyte(&b, ' ');
        print(&b, v->compactat);
        putbyte(&b, ' ');
        print(&b, v->tokens);
        putbyte(&b, ' ');
        print(&b, v->cached);
        expect(t, "gauge.usage", finish(&b), "131072 104857 12345 11000");
        b32 changed = apply(v, a, R"({"sid":"s","ts":0,"ev":"live","state":"idle","tokens":20000})");
        expect(t, "gauge.live", istr(&a, changed*100000 + v->tokens), "120000");
        changed = apply(v, a, R"({"sid":"s","ts":0,"ev":"live","state":"idle"})");
        expect(t, "gauge.kept", istr(&a, changed*1000000 + v->ctx), "131072");
        apply(v, a, R"({"sid":"s","seq":5,"ts":0,"ev":"turn.end","turn":1,"why":"done",)"
                    R"("ctx":200000,"compact_at":160000,"tokens":21000})");
        expect(t, "gauge.turnend", istr(&a, v->ctx + v->compactat + v->tokens), "381000");
    }

    // A code block's Copy: its code byte for byte, as selecting the
    // block whole (a triple click) copies it, without its last newline
    {
        Arena a  = perm;
        View *v  = testview(&a);
        Str   md = "Text.\n\n```c\nint\tx;  \n\ty;\n```\n\nAfter.\n";
        addmsg(v, VIEW_ASSISTANT, md);
        Doc *d  = msgdoc(v, 0);
        i32  cb = -1;
        for (i32 k = 0; k < d->blocks.len; k++) {
            cb = d->blocks[k].type==DOC_CODE ? k : cb;
        }
        expect(t, "copyblock", copyblock(v, &a, 0, cb), "int\tx;  \n\ty;");
        i32 c0 = (i32)find(md, "int");
        selbegin(v, {0, c0+2}, SEL_BLOCK);
        expect(t, "copyblock.triple", copysel(v, &a, COPY_TEXT), "int\tx;  \n\ty;");
    }

    // A new width keeps the top edge's line: the anchor found at one
    // width, put back at another, finds the same message, the line
    // holding the same offset, and the same distance into it
    {
        Arena   a = perm;
        View   *v = testview(&a);
        Measure m = fixedmeasure();
        v->extra = 30;
        v->gap   = 4;
        Buf para(&a, 4096);
        for (i32 i = 0; i < 60; i++) {
            print(&para, "word");
            print(&para, (i64)i);
            putbyte(&para, ' ');
        }
        Str p = finish(&para);
        for (i32 i = 0; i < 6; i++) {
            addmsg(v, VIEW_ASSISTANT, p);
        }
        f32 head = 20;
        for (i32 i = 0; i < 6; i++) msglayout(v, i, &m, 400, 1);
        viewtops(v, 400, 10, 20);
        f64    y  = v->msgs[3].top + head + 3*20 + 7;
        Anchor an = viewanchor(v, &m, 400, 1, head, y);
        Doc   *d  = msgdoc(v, 3);
        expect(t, "anchor.msg", istr(&a, an.msg*1000 + (i32)an.dy), "3007");
        expect(t, "anchor.line", istr(&a, an.off>0 && msgy(d, &v->msgs[3].layout, an.off)==60), "1");

        v->topsdirty = 1;
        viewtops(v, 250, 10, 20);  // above it, estimates at the new width
        msglayout(v, 3, &m, 250, 1);
        viewtops(v, 250, 10, 20);
        f64        y2 = anchory(v, an, &m, 250, 1, head);
        Anchor     b  = viewanchor(v, &m, 250, 1, head, y2);
        MsgLayout *ml = &v->msgs[3].layout;
        expect(t, "anchor.again", istr(&a, b.msg*1000 + (i32)b.dy), "3007");
        expect(t, "anchor.same", istr(&a, msgy(d, ml, b.off)==msgy(d, ml, an.off) && b.off<=an.off), "1");

        // In a header: the distance from the message's top
        Anchor h = viewanchor(v, &m, 250, 1, head, v->msgs[2].top + 5);
        expect(t, "anchor.header", istr(&a, h.msg*1000 + h.off*100 + (i32)h.dy), "1905");
        expect(t, "anchor.headery", istr(&a, (i64)(anchory(v, h, &m, 250, 1, head) - v->msgs[2].top)), "5");
    }
}
