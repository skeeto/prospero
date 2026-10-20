// View model tests: events to messages, virtualization, selection, copy.

static View *testview(Arena *a)
{
    Os *os = newos(a, 1<<12);
    iz  mb = (iz)1<<20;
    return newview(a, os_reserve(os, 16*mb), os_reserve(os, 16*mb),
                   os_reserve(os, 64*mb), 32*mb);
}

static Str msgsshow(Arena *a, View *v)
{
    Buf b(a, 256);
    static char const roles[] = "UATXN";
    for (iz i = 0; i < v->msgs.len; i++) {
        ViewMsg *m = &v->msgs[i];
        putbyte(&b, (u8)roles[m->role]);
        if (!m->seq) putbyte(&b, '~');
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
               "U:hi;T:hm;A:Hello;X:read {\"path\":\"x\"};");
        // The committed texts take the provisional ones' place
        expect(t, "view.reclaimed", istr(&a, v->text.beg - prov), "24");
        apply(v, a, R"({"sid":"s","seq":4,"ts":0,"ev":"turn.end","turn":1,"why":"done"})");
        expect(t, "view.idle", istr(&a, v->busy), "0");
        b32 changed = apply(v, a, R"({"sid":"s","ts":0,"ev":"progress","turn":1})");
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
        expect(t, "view.dropped", msgsshow(&a, v), "U:q;");
        Range r = selrange(&v->sel);
        expect(t, "view.clamped", istr(&a, r.end.msg*100 + r.end.off), "1");
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
}
