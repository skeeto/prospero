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

static void test_a11y(Test *t)
{
    Arena a = t->perm;
    A11y *x = newa11y(&a, carve(&a, 1<<20), carve(&a, 1<<20));

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
    byte *hi[2] = {x->mem[0].beg, x->mem[1].beg};
    for (i32 frame = 0; frame < 10; frame++) {
        b = axbegin(x);
        for (u32 k = 0; k < 2000; k++) {
            axadd(b, k ? 100 + (k-1)/8 : 0, 100+k, AX_BUTTON, "n");
        }
        axpublish(x, 0);
    }
    expect(t, "a11y.reuse", istr(&a, x->mem[0].beg<=hi[0] && x->mem[1].beg<=hi[1]), "1");
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
