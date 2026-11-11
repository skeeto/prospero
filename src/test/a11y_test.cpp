// Accessibility tree tests: a tree built and dumped, the events between
// two trees, parents missing or in a cycle, lint, names from labels and
// icons, and both arenas reused frame after frame.

static Str axeventsstr(Arena *a, Slice<AxEvent> e)
{
    static Str const kinds[] = {
        "destroyed", "created", "children", "name", "value", "state",
        "focus", "layout",
    };
    Buf b(a, 256);
    for (iz i = 0; i < e.len; i++) {
        if (i) {
            putbyte(&b, ' ');
        }
        print(&b, kinds[e[i].kind]);
        putbyte(&b, ':');
        print(&b, (i64)e[i].key);
    }
    return finish(&b);
}

static Str axdumpstr(Arena *a, AxTree *t, b32 rects)
{
    Buf b(a, 1024);
    axdump(&b, t, rects);
    return finish(&b);
}

static AxNode *axitem(AxBuild *b, u32 parent, u32 key, AxRole role, Str name,
                      f32 x, f32 y, u32 actions)
{
    AxNode *n = axadd(b, parent, key, role, name);
    n->rect    = {x, y, x+60, y+20};
    n->actions = actions;
    return n;
}

static A11y *newtesta11y(Arena *a)
{
    Arena mem[AXM_N];
    for (i32 i = 0; i < AXM_N; i++) {
        mem[i] = carve(a, 1<<20);
    }
    return newa11y(a, mem);
}

static void test_a11y(Test *t)
{
    Arena a = t->perm;
    A11y *x = newtesta11y(&a);

    // An empty tree before the first frame
    expect(t, "a11y.empty", axdumpstr(&a, x->pub, 1), "");
    expect(t, "a11y.emptyfind", istr(&a, axfind(x->pub, 10)), "-1");

    // The first frame: a pane with a prompt, a button, a checkbox
    AxBuild *b = axbegin(x);
    AxNode  *w = axadd(b, 0, 10, AX_WINDOW, "Session");
    w->rect = {0, 0, 800, 600};
    AxNode *p = axitem(b, 10, 11, AX_TEXTAREA, "Prompt", 10, 500, AXA_EDIT|AXA_FOCUS);
    axsetvalue(b, p, "hello \"you\"\nthere");
    axsethelp(b, axitem(b, 10, 12, AX_BUTTON, "Send", 700, 500, AXA_PRESS), "Send it (Enter)");
    axitem(b, 10, 13, AX_CHECKBOX, "Plan", 700, 530, AXA_TOGGLE);
    Slice<AxEvent> e = axpublish(x, 11);
    expect(t, "a11y.dump", axdumpstr(&a, x->pub, 1),
        "app\n"
        "  window \"Session\" @0,0 800x600\n"
        "    textarea \"Prompt\" = \"hello \\\"you\\\"\\nthere\" [focused] @10,500 60x20\n"
        "    button \"Send\" ?\"Send it (Enter)\" @700,500 60x20\n"
        "    checkbox \"Plan\" @700,530 60x20\n");
    expect(t, "a11y.first", axeventsstr(&a, e),
        "created:10 created:11 created:12 created:13 focus:11 layout:0");
    expect(t, "a11y.find", istr(&a, axfind(x->pub, 12)), "3");
    expect(t, "a11y.nchild", istr(&a, x->pub->nodes[1].nchild), "3");

    // The same again: nothing changed
    b = axbegin(x);
    w = axadd(b, 0, 10, AX_WINDOW, "Session");
    w->rect = {0, 0, 800, 600};
    p = axitem(b, 10, 11, AX_TEXTAREA, "Prompt", 10, 500, AXA_EDIT|AXA_FOCUS);
    axsetvalue(b, p, "hello \"you\"\nthere");
    axsethelp(b, axitem(b, 10, 12, AX_BUTTON, "Send", 700, 500, AXA_PRESS), "Send it (Enter)");
    axitem(b, 10, 13, AX_CHECKBOX, "Plan", 700, 530, AXA_TOGGLE);
    e = axpublish(x, 11);
    expect(t, "a11y.same", axeventsstr(&a, e), "");

    // Typed, checked, Send now Stop, a new button, focus on it
    b = axbegin(x);
    w = axadd(b, 0, 10, AX_WINDOW, "Session");
    w->rect = {0, 0, 800, 600};
    p = axitem(b, 10, 11, AX_TEXTAREA, "Prompt", 10, 500, AXA_EDIT|AXA_FOCUS);
    axsetvalue(b, p, "hello");
    axitem(b, 10, 12, AX_BUTTON, "Stop", 700, 500, AXA_PRESS);
    axitem(b, 10, 13, AX_CHECKBOX, "Plan", 700, 530, AXA_TOGGLE)->state |= AXS_CHECKED;
    axitem(b, 10, 14, AX_BUTTON, "Attach", 640, 500, AXA_PRESS);
    e = axpublish(x, 14);
    expect(t, "a11y.changes", axeventsstr(&a, e),
        "created:14 children:10 value:11 name:12 state:13 focus:14 layout:0");

    // The checkbox gone, the button moved, no focus
    b = axbegin(x);
    w = axadd(b, 0, 10, AX_WINDOW, "Session");
    w->rect = {0, 0, 800, 600};
    p = axitem(b, 10, 11, AX_TEXTAREA, "Prompt", 10, 500, AXA_EDIT|AXA_FOCUS);
    axsetvalue(b, p, "hello");
    axitem(b, 10, 12, AX_BUTTON, "Stop", 700, 500, AXA_PRESS);
    axitem(b, 10, 14, AX_BUTTON, "Attach", 600, 500, AXA_PRESS);
    e = axpublish(x, 0);
    expect(t, "a11y.gone", axeventsstr(&a, e), "destroyed:13 children:10 focus:0 layout:0");
    expect(t, "a11y.gonefind", istr(&a, axfind(x->pub, 13)), "-1");

    // Clipped away is a layout change only
    b = axbegin(x);
    axadd(b, 0, 10, AX_WINDOW, "Session")->rect = {0, 0, 800, 600};
    axitem(b, 10, 11, AX_TEXTAREA, "Prompt", 10, 500, AXA_EDIT|AXA_FOCUS)->value = "hello";
    axitem(b, 10, 12, AX_BUTTON, "Stop", 700, 500, AXA_PRESS);
    axitem(b, 10, 14, AX_BUTTON, "Attach", 600, 500, AXA_PRESS)->state |= AXS_OFFSCREEN;
    e = axpublish(x, 0);
    expect(t, "a11y.offscreen", axeventsstr(&a, e), "layout:0");

    // Children before their parent, a parent never added, a cycle, a
    // key twice: all still in the tree
    b = axbegin(x);
    axadd(b, 20, 21, AX_BUTTON, "Early");
    axadd(b, 0, 20, AX_WINDOW, "Late");
    axadd(b, 99, 22, AX_BUTTON, "Orphan");
    axadd(b, 24, 23, AX_GROUP, "A");
    axadd(b, 23, 24, AX_GROUP, "B");
    axadd(b, 20, 21, AX_BUTTON, "Twin");
    axadd(b, 20, 0, AX_TEXT, "Keyless");
    axpublish(x, 0);
    expect(t, "a11y.links", axdumpstr(&a, x->pub, 0),
        "app\n"
        "  window \"Late\"\n"
        "    button \"Early\"\n"
        "    button \"Twin\"\n"
        "    text \"Keyless\"\n"
        "  button \"Orphan\"\n"
        "  group \"A\"\n"
        "    group \"B\"\n");
    expect(t, "a11y.dups", istr(&a, x->pub->dups), "1");
    expect(t, "a11y.twinfind", istr(&a, axfind(x->pub, 21)), "1");

    // Lint: on screen, actionable, unnamed
    b = axbegin(x);
    axitem(b, 0, 30, AX_BUTTON, "", 0, 0, AXA_PRESS);
    axitem(b, 0, 31, AX_BUTTON, "", 0, 0, AXA_PRESS)->state |= AXS_OFFSCREEN;
    axitem(b, 0, 32, AX_NONE, "", 0, 0, AXA_PRESS);
    axitem(b, 0, 33, AX_TEXT, "", 0, 0, 0);
    axitem(b, 0, 34, AX_BUTTON, "Named", 0, 0, AXA_PRESS);
    axitem(b, 0, 35, AX_TEXTFIELD, "", 0, 0, AXA_EDIT);
    axpublish(x, 0);
    Slice<i32> lint = axlint(&a, x->pub);
    Buf lb(&a, 64);
    for (iz i = 0; i < lint.len; i++) {
        print(&lb, (i64)x->pub->nodes[lint[i]].key);
        putbyte(&lb, ' ');
    }
    expect(t, "a11y.lint", finish(&lb), "30 35 ");

    // Many nodes, past the first capacity, each found by its key
    for (i32 frame = 0; frame < 3; frame++) {
        b = axbegin(x);
        for (u32 k = 0; k < 2000; k++) {
            axadd(b, k ? 100 + (k-1)/8 : 0, 100+k, AX_BUTTON, "n");
        }
        axpublish(x, 0);
    }
    b32 found = 1;
    for (u32 k = 0; k < 2000; k++) {
        i32 i = axfind(x->pub, 100+k);
        found &= i>=0 && x->pub->nodes[i].key==100+k;
    }
    expect(t, "a11y.manyfind", istr(&a, found), "1");
    expect(t, "a11y.manylen", istr(&a, x->pub->len), "2001");

    // Frame after frame, the arenas are reused, not grown
    byte *hi[2] = {x->mem[AXM_TREE0].beg, x->mem[AXM_TREE1].beg};
    for (i32 frame = 0; frame < 10; frame++) {
        b = axbegin(x);
        for (u32 k = 0; k < 2000; k++) {
            axadd(b, k ? 100 + (k-1)/8 : 0, 100+k, AX_BUTTON, "n");
        }
        axpublish(x, 0);
    }
    expect(t, "a11y.reuse", istr(&a, x->mem[AXM_TREE0].beg<=hi[0] && x->mem[AXM_TREE1].beg<=hi[1]), "1");
}

static void test_a11yname(Test *t)
{
    Arena a = t->perm;
    expect(t, "a11yname.plain", axlabel(&a, "Send"), "Send");
    expect(t, "a11yname.hidden", axlabel(&a, "Send##prompt"), "Send");
    expect(t, "a11yname.idonly", axlabel(&a, "##prompt"), "");
    expect(t, "a11yname.trim", axlabel(&a, "  Plan mode "), "Plan mode");
    expect(t, "a11yname.lead", axlabel(&a, ICON_SEND " Send"), "Send");
    expect(t, "a11yname.inner", axlabel(&a, "Copy " ICON_COPY " now"), "Copy now");
    expect(t, "a11yname.icon", axlabel(&a, ICON_CLOSE "##close"), "Close");
    expect(t, "a11yname.icons", axlabel(&a, ICON_ADD ICON_FOLDER), "Add Folder");
    expect(t, "a11yname.unknown", axlabel(&a, "\xee\x80\x80"), "");  // U+E000
    expect(t, "a11yname.utf8", axlabel(&a, "Caf\xc3\xa9 " ICON_ADD), "Caf\xc3\xa9");

    // Every icon has a name
    Str used = concat(&a, fonticonsused, ICON_SHELL);
    Buf missing(&a, 64);
    for (iz i = 0; i < used.len;) {
        i32 c = utf8decode(used, &i);
        if (!axiconname(c).len) {
            printhex(&missing, (u64)c, 4);
            putbyte(&missing, ' ');
        }
    }
    expect(t, "a11yname.everyicon", finish(&missing), "");
}

// Geometry as a monospace field would draw s, every line showing, each
// code point 10 wide (CR and LF none), at 100,50 with lines 20 high.
static AxGeom axtestgeom(Arena *a, Str s)
{
    AxGeom g = {};
    g.x      = 100;
    g.y      = 50;
    g.lineh  = 20;
    g.clip   = {0, 0, 1000, 1000};
    g.lines  = alloc<i32>(a, s.len+1);
    g.xs     = alloc<f32>(a, s.len*2+2);
    g.lines[g.nlines++] = 0;
    for (iz i = 0; i < s.len; i++) {
        if (s[i] == '\n') {
            g.lines[g.nlines++] = (i32)i+1;
        }
    }
    g.vis1 = g.nlines;
    for (i32 k = 0; k < g.nlines; k++) {
        i32 e  = k+1<g.nlines ? g.lines[k+1] : (i32)s.len;
        f32 px = 0;
        for (iz i = g.lines[k]; i < e;) {
            iz  beg = i;
            i32 c   = utf8decode(s, &i);
            for (iz j = beg; j < i; j++) {
                g.xs[g.nxs++] = px;
            }
            px += c=='\n' || c=='\r' ? 0 : 10;
        }
        g.xs[g.nxs++] = px;
    }
    return g;
}

static void axtestsync(A11y *x, Arena *a, u32 state, b32 multiline, Str s,
                       i32 anchor, i32 caret)
{
    AxGeom g = axtestgeom(a, s);
    axtextsync(x, 7, 77, state, multiline, s, anchor, caret, &g);
}

static Str axspanstr(Arena *a, AxSpan s)
{
    Buf b(a, 32);
    print(&b, (i64)s.lo);
    putbyte(&b, '-');
    print(&b, (i64)s.hi);
    return finish(&b);
}

static Str axrectstr(Arena *a, AxRect r)
{
    Buf b(a, 64);
    print(&b, axround(r.x0));
    putbyte(&b, ',');
    print(&b, axround(r.y0));
    putbyte(&b, ' ');
    print(&b, axround(r.x1));
    putbyte(&b, ',');
    print(&b, axround(r.y1));
    return finish(&b);
}

static Str axeditstr(Arena *a, A11y *x, u32 id, Str text, i64 now)
{
    AxEdit e;
    if (!axnextedit(x, id, text, now, &e)) {
        return "none";
    }
    Buf b(a, 64);
    print(&b, (i64)e.lo);
    putbyte(&b, '-');
    print(&b, (i64)e.hi);
    putbyte(&b, ' ');
    axquote(&b, e.text);
    putbyte(&b, ' ');
    print(&b, (i64)e.anchor);
    putbyte(&b, ':');
    print(&b, (i64)e.caret);
    return finish(&b);
}

static void test_a11ytext(Test *t)
{
    Arena a = t->perm;
    A11y *x = newtesta11y(&a);

    // UTF-8, UTF-16 and code points: an emoji past U+FFFF is a
    // surrogate pair, a CJK character one unit
    Str s = "a\xf0\x9f\x98\x80" "b\xe4\xb8\xad\n";  // a😀b中\n
    axtestsync(x, &a, 0, 1, s, 0, 0);
    expect(t, "a11ytext.len",     istr(&a, axunitlen(x, AXU_BYTE)),  "10");
    expect(t, "a11ytext.len16",   istr(&a, axunitlen(x, AXU_UTF16)), "6");
    expect(t, "a11ytext.lencp",   istr(&a, axunitlen(x, AXU_CP)),    "5");
    expect(t, "a11ytext.to16",    istr(&a, axtounit(x, AXU_UTF16, 5)), "3");
    expect(t, "a11ytext.to16mid", istr(&a, axtounit(x, AXU_UTF16, 3)), "1");
    expect(t, "a11ytext.to16cjk", istr(&a, axtounit(x, AXU_UTF16, 6)), "4");
    expect(t, "a11ytext.tocp",    istr(&a, axtounit(x, AXU_CP, 9)),    "4");
    expect(t, "a11ytext.lowhalf", istr(&a, axtobyte(x, AXU_UTF16, 2)), "1");
    expect(t, "a11ytext.from16",  istr(&a, axtobyte(x, AXU_UTF16, 4)), "6");
    expect(t, "a11ytext.fromcp",  istr(&a, axtobyte(x, AXU_CP, 3)),    "6");
    expect(t, "a11ytext.snap",    istr(&a, axtobyte(x, AXU_BYTE, 7)),  "6");
    expect(t, "a11ytext.past",    istr(&a, axtobyte(x, AXU_UTF16, 99)), "10");
    expect(t, "a11ytext.lines",   istr(&a, axlinecount(x)), "2");
    expect(t, "a11ytext.lastline", istr(&a, axlineat(x, 10)), "1");

    // Geometry: the emoji is one character, 10 wide, and a point on
    // its right half is after it
    expect(t, "a11ytext.rect0", axrectstr(&a, axcharrect(x, 0)), "100,50 110,70");
    expect(t, "a11ytext.rectemoji", axrectstr(&a, axcharrect(x, 3)), "110,50 120,70");
    expect(t, "a11ytext.rectnl", axrectstr(&a, axcharrect(x, 9)), "140,50 140,70");
    expect(t, "a11ytext.rectend", axrectstr(&a, axcharrect(x, 10)), "100,70 100,90");
    expect(t, "a11ytext.hitleft", istr(&a, axoffsetat(x, 114, 55)), "1");
    expect(t, "a11ytext.hitright", istr(&a, axoffsetat(x, 117, 55)), "5");
    expect(t, "a11ytext.hitpast", istr(&a, axoffsetat(x, 900, 55)), "9");
    expect(t, "a11ytext.hitbelow", istr(&a, axoffsetat(x, 0, 900)), "10");

    // Words and paragraphs
    Str w = "hello, world  foo_bar\nnext";
    expect(t, "a11ytext.word0",     axspanstr(&a, axwordat(w, 0)),  "0-5");
    expect(t, "a11ytext.wordin",    axspanstr(&a, axwordat(w, 3)),  "0-5");
    expect(t, "a11ytext.wordpunct", axspanstr(&a, axwordat(w, 5)),  "5-6");
    expect(t, "a11ytext.wordspace", axspanstr(&a, axwordat(w, 12)), "12-14");
    expect(t, "a11ytext.wordunder", axspanstr(&a, axwordat(w, 16)), "14-21");
    expect(t, "a11ytext.wordend",   axspanstr(&a, axwordat(w, 26)), "22-26");
    expect(t, "a11ytext.wordnone",  axspanstr(&a, axwordat("", 0)), "0-0");
    expect(t, "a11ytext.wordcjk",   axspanstr(&a, axwordat(s, 6)),  "0-9");
    expect(t, "a11ytext.para0",     axspanstr(&a, axparaat(w, 3)),  "0-22");
    expect(t, "a11ytext.para1",     axspanstr(&a, axparaat(w, 23)), "22-26");

    // An edit: shadowed at once, CRLF and tabs cleaned, queued for
    // ImGui with the bytes it replaces
    axtestsync(x, &a, 0, 1, "hello world", 11, 11);
    i64 rev = x->text.rev;
    expect(t, "a11ytext.edit", istr(&a, axedit(x, 5, 11, " there\r\nfriend\t!", AX_AFTER, AX_AFTER, 1000)), "1");
    expect(t, "a11ytext.shadow", x->text.text, "hello there\nfriend !");
    expect(t, "a11ytext.shadowsel", istr(&a, x->text.caret), "20");
    expect(t, "a11ytext.shadowrev", istr(&a, x->text.rev > rev), "1");
    expect(t, "a11ytext.shadowlines", istr(&a, axlinecount(x)), "2");

    // ImGui's text, until it takes the edit, doesn't undo the shadow
    axtestsync(x, &a, 0, 1, "hello world", 11, 11);
    expect(t, "a11ytext.ahead", x->text.text, "hello there\nfriend !");
    expect(t, "a11ytext.stale", axrectstr(&a, axcharrect(x, 13)), "100,70 100,90");
    expect(t, "a11ytext.other", axeditstr(&a, x, 78, "hello world", 1000), "none");
    expect(t, "a11ytext.take", axeditstr(&a, x, 77, "hello world", 1000),
           "5-11 \" there\\nfriend !\" 20:20");
    expect(t, "a11ytext.taken", axeditstr(&a, x, 77, "hello there\nfriend !", 1000), "none");
    expect(t, "a11ytext.alltaken", istr(&a, axqueued(x, 77)), "0");
    rev = x->text.rev;
    axtestsync(x, &a, 0, 1, "hello there\nfriend !", 20, 20);
    expect(t, "a11ytext.caughtup", istr(&a, x->text.rev), istr(&a, rev));
    expect(t, "a11ytext.fresh", axrectstr(&a, axcharrect(x, 13)), "110,70 120,90");

    // Stale: typed keys changed the bytes, so it and the next go. The
    // queue, all taken, starts over.
    axtestsync(x, &a, 0, 1, "hello world", 0, 0);
    axedit(x, 0, 5, "HELLO", 0, 5, 1000);
    expect(t, "a11ytext.emptied", istr(&a, x->mem[AXM_QUEUE].beg - x->base[AXM_QUEUE] < 2048), "1");
    axedit(x, 6, 11, "WORLD", AX_AFTER, AX_AFTER, 1000);
    expect(t, "a11ytext.twoedits", x->text.text, "HELLO WORLD");
    expect(t, "a11ytext.changed", axeditstr(&a, x, 77, "jello world", 1000), "none");
    expect(t, "a11ytext.dropped", axeditstr(&a, x, 77, "jello world", 1000), "none");
    axtestsync(x, &a, 0, 1, "jello world", 1, 1);
    expect(t, "a11ytext.resync", x->text.text, "jello world");

    // Waiting too long for its field: it and the edits counting on it go
    axedit(x, 0, 0, ">", AX_AFTER, AX_AFTER, 0);
    axedit(x, 1, 1, "<", AX_AFTER, AX_AFTER, AXEDIT_TTL);
    expect(t, "a11ytext.expired", axeditstr(&a, x, 77, "jello world", AXEDIT_TTL+1), "none");
    expect(t, "a11ytext.expiredgone", istr(&a, axqueued(x, 77)), "0");

    // An insertion, after a key typed first: the length tells
    axtestsync(x, &a, 0, 1, "hello", 0, 0);
    axedit(x, 5, 5, "x", AX_AFTER, AX_AFTER, 0);
    expect(t, "a11ytext.typedfirst", axeditstr(&a, x, 77, "Qhello", 0), "none");
    axtestsync(x, &a, 0, 1, "Qhello", 1, 1);
    expect(t, "a11ytext.typedtext", x->text.text, "Qhello");

    // Bytes not UTF-8 are U+FFFD, as ImGui takes them
    axedit(x, 6, 6, "\xff!", AX_AFTER, AX_AFTER, 0);
    expect(t, "a11ytext.notutf8", x->text.text, "Qhello\xef\xbf\xbd!");
    axeditstr(&a, x, 77, "Qhello", 0);

    // Ranges not in the text; a selection set with the caret given
    // after the text as it was put in, before cleaning
    axtestsync(x, &a, 0, 1, "hello world", 0, 0);
    expect(t, "a11ytext.backwards", istr(&a, axedit(x, 3, 2, "", 0, 0, 0)), "0");
    expect(t, "a11ytext.beyond", istr(&a, axedit(x, 0, 12, "", 0, 0, 0)), "0");
    expect(t, "a11ytext.negative", istr(&a, axedit(x, -1, 0, "", 0, 0, 0)), "0");
    axedit(x, 0, 0, "ab\r\ncd", 2, 6, 0);
    expect(t, "a11ytext.crlf", axeditstr(&a, x, 77, "hello world", 0), "0-0 \"ab\\ncd\" 2:5");
    axselect(x, 1, 3, 0);
    expect(t, "a11ytext.select", axeditstr(&a, x, 77, "ab\ncdhello world", 0), "-1-0 \"\" 1:3");
    expect(t, "a11ytext.selected", axspanstr(&a, axselection(&x->text)), "1-3");

    // One line: newlines dropped
    axtestsync(x, &a, 0, 0, "name", 4, 4);
    axedit(x, 4, 4, " two\nlines", AX_AFTER, AX_AFTER, 0);
    expect(t, "a11ytext.oneline", x->text.text, "name twolines");
    expect(t, "a11ytext.onelineedit", axeditstr(&a, x, 77, "name", 0), "4-4 \" twolines\" 13:13");

    // Edits for a field that turned read-only, lost the focus, or gave
    // way to another are dropped, and its text is ImGui's again
    axtestsync(x, &a, 0, 1, "draft", 5, 5);
    axedit(x, 5, 5, "!", AX_AFTER, AX_AFTER, 0);
    axtestsync(x, &a, AXS_READONLY, 1, "draft", 5, 5);
    expect(t, "a11ytext.nowreadonly", istr(&a, axqueued(x, 77)), "0");
    expect(t, "a11ytext.nowreadonlytext", x->text.text, "draft");
    axtestsync(x, &a, 0, 1, "draft", 5, 5);
    axedit(x, 5, 5, "!", AX_AFTER, AX_AFTER, 0);
    axtextnone(x);
    expect(t, "a11ytext.blurred", istr(&a, axqueued(x, 77)), "0");
    axtestsync(x, &a, 0, 1, "draft", 5, 5);
    axedit(x, 5, 5, "!", AX_AFTER, AX_AFTER, 0);
    AxGeom og = axtestgeom(&a, "other");
    axtextsync(x, 7, 78, 0, 1, "other", 0, 0, &og);  // the same key: a new session's
    expect(t, "a11ytext.switched", istr(&a, axqueued(x, 77)), "0");
    expect(t, "a11ytext.switchedtext", x->text.text, "other");

    // Read-only and password fields refuse edits
    axtestsync(x, &a, AXS_READONLY, 1, "fixed", 0, 0);
    expect(t, "a11ytext.readonly", istr(&a, axedit(x, 0, 0, "x", 0, 0, 0)), "0");
    expect(t, "a11ytext.readonlysel", istr(&a, axselect(x, 0, 1, 0)), "0");
    axtextnone(x);
    expect(t, "a11ytext.none", istr(&a, axedit(x, 0, 0, "x", 0, 0, 0)), "0");

    // Deleting the emoji by its UTF-16 range
    axtestsync(x, &a, 0, 1, "a\xf0\x9f\x98\x80" "b", 0, 0);
    axedit(x, axtobyte(x, AXU_UTF16, 1), axtobyte(x, AXU_UTF16, 3), "", AX_AFTER, AX_AFTER, 0);
    expect(t, "a11ytext.emojigone", x->text.text, "ab");
    expect(t, "a11ytext.emojiedit", axeditstr(&a, x, 77, "a\xf0\x9f\x98\x80" "b", 0), "1-5 \"\" 1:1");

    // Marked text in place of the selection, the document around it
    axtestsync(x, &a, 0, 1, "hello world", 6, 11);
    axmark(x, "wor", 3, 3);
    expect(t, "a11ytext.doc", axdoc(x, &a), "hello wor");
    expect(t, "a11ytext.doclen", istr(&a, axdoclen(x, AXU_UTF16)), "9");
    expect(t, "a11ytext.doctext", istr(&a, axdoctext(x, 8)), "6");
    expect(t, "a11ytext.docafter", istr(&a, axdoctext(x, 9)), "11");
    axmark(x, "\xf0\x9f\x98\x80x", 5, 5);
    expect(t, "a11ytext.doc16", istr(&a, axdoclen(x, AXU_UTF16)), "9");
    expect(t, "a11ytext.docbyte", istr(&a, axdocbyte(x, AXU_UTF16, 8)), "10");
    expect(t, "a11ytext.docunit", istr(&a, axdocunit(x, AXU_UTF16, 10)), "8");
    expect(t, "a11ytext.docmid", istr(&a, axdocbyte(x, AXU_UTF16, 7)), "6");

    // It stays while the field's text changes, and goes with the field
    axtestsync(x, &a, 0, 1, "hello world!", 6, 11);
    expect(t, "a11ytext.markkept", axdoc(x, &a), "hello \xf0\x9f\x98\x80x!");
    AxGeom g = axtestgeom(&a, "other");
    axtextsync(x, 8, 88, 0, 1, "other", 0, 0, &g);
    expect(t, "a11ytext.markgone", axdoc(x, &a), "other");

    // Marked text's rects are where it was drawn, over the field; the
    // text's around it are where the text was drawn, under it
    AxGeom mg = axtestgeom(&a, "ab\ncd");
    f32    mx[] = {110, 110, 110, 125, 130};
    f32    my[] = {50, 50, 50, 50, 50};
    mg.mxs = mx;
    mg.mys = my;
    mg.nm  = 5;
    axtextsync(x, 7, 77, 0, 1, "ab\ncd", 1, 1, &mg);
    axmark(x, "\xe3\x81\x8bx", 4, 4);  // U+304B
    expect(t, "a11ytext.markdoc", axdoc(x, &a), "a\xe3\x81\x8bxb\ncd");
    expect(t, "a11ytext.docrect0", axrectstr(&a, axdocrect(x, 0)), "100,50 110,70");
    expect(t, "a11ytext.docrectka", axrectstr(&a, axdocrect(x, 2)), "110,50 125,70");
    expect(t, "a11ytext.docrectx", axrectstr(&a, axdocrect(x, 4)), "125,50 130,70");
    expect(t, "a11ytext.docrectafter", axrectstr(&a, axdocrect(x, 5)), "110,50 120,70");
    expect(t, "a11ytext.docrectline", axrectstr(&a, axdocrect(x, 7)), "100,70 110,90");
    Str was = x->text.marked;
    axmark(x, "\xe3\x81\x8bx", 4, 4);
    expect(t, "a11ytext.markagain", istr(&a, was.data==x->text.marked.data), "1");
    axtestsync(x, &a, 0, 1, "ab\ncd", 1, 1);  // not drawn: where it goes
    expect(t, "a11ytext.docrectundrawn", axrectstr(&a, axdocrect(x, 2)), "110,50 110,70");

    // The caret at a wrap: on the line ImGui drew it on
    AxGeom wg = {};
    i32    wl[] = {0, 3};
    f32    wx[] = {0, 10, 20, 30, 0, 10, 20, 30};
    wg.x      = 100;
    wg.y      = 50;
    wg.lineh  = 20;
    wg.lines  = wl;
    wg.nlines = 2;
    wg.vis1   = 2;
    wg.xs     = wx;
    wg.nxs    = 8;
    wg.caretline = 1;
    axtextsync(x, 7, 77, 0, 1, "abcdef", 3, 3, &wg);
    expect(t, "a11ytext.caretnext", axrectstr(&a, axcaretrect(x)), "100,70 100,90");
    wg.caretline = 0;
    axtextsync(x, 7, 77, 0, 1, "abcdef", 3, 3, &wg);
    expect(t, "a11ytext.caretwrap", axrectstr(&a, axcaretrect(x)), "130,50 130,70");
    expect(t, "a11ytext.charwrap", axrectstr(&a, axcharrect(x, 3)), "100,70 110,90");
    expect(t, "a11ytext.hitwrap", istr(&a, axoffsetat(x, 900, 55)), "3");

    // A hit past a line's end stays before its CRLF
    axtestsync(x, &a, 0, 1, "ab\r\ncd", 0, 0);
    expect(t, "a11ytext.hitcrlf", istr(&a, axoffsetat(x, 900, 55)), "2");
}
