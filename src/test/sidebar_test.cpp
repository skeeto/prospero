// Sessions sidebar tests: the tree's rows from sessions on two hosts,
// grouped across them, archived, subagents under their parents, and
// what folds and what stays open.

// The rows, one a line: indentation, then G group, H host, S sid or A
// for Archived, with + folded, ! open regardless, @ away, and the count
// of a heading.
static Str siderows(Arena *a, SideIn *in, Slice<SideRow> rows)
{
    Buf b(a, 512);
    for (iz i = 0; i < rows.len; i++) {
        SideRow *r = &rows[i];
        for (i32 d = 0; d < r->depth; d++) {
            putbyte(&b, ' ');
        }
        switch (r->kind) {
        case SROW_GROUP:
            print(&b, "G ");
            print(&b, r->group);
            break;
        case SROW_HOST:
            print(&b, "H ");
            print(&b, in->hosts[r->host].key);
            break;
        case SROW_SESSION:
            print(&b, "S ");
            print(&b, in->sess[r->sess].sid);
            break;
        case SROW_ARCHIVED:
            print(&b, "A");
            break;
        }
        if (r->kind != SROW_SESSION) {
            putbyte(&b, ' ');
            print(&b, (i64)r->count);
        }
        print(&b, r->folded ? Str("+") : Str(""));
        print(&b, r->forced ? Str("!") : Str(""));
        print(&b, r->away ? Str("@") : Str(""));
        putbyte(&b, '\n');
    }
    return finish(&b);
}

static SideSess sidesess(i32 host, Str sid, Str parent, Str group, b32 archived)
{
    SideSess s = {};
    s.host     = host;
    s.sid      = sid;
    s.parent   = parent;
    s.group    = group;
    s.archived = archived;
    return s;
}

static void test_sidebar(Test *t)
{
    Arena a = t->perm;

    // One host: no headings, the newest first, subagents oldest first
    SideIn in = {};
    in.hosts = push(&a, in.hosts, SideHost{"this", 1});
    in.sess  = push(&a, in.sess, sidesess(0, "s3", {}, {}, 0));
    in.sess  = push(&a, in.sess, sidesess(0, "s2b", "s2", {}, 0));
    in.sess  = push(&a, in.sess, sidesess(0, "s2a", "s2", {}, 0));
    in.sess  = push(&a, in.sess, sidesess(0, "s2", {}, {}, 0));
    in.sess  = push(&a, in.sess, sidesess(0, "s1", {}, {}, 0));
    expect(t, "sidebar.flat", siderows(&a, &in, sidetree(&a, &in)),
           "S s3\nS s2\n S s2a\n S s2b\nS s1\n");

    // While a session is dragged, Archived is there to drop it on
    in.archive = 1;
    Slice<SideRow> drag = sidetree(&a, &in);
    expect(t, "sidebar.dragarchive", siderows(&a, &in, drag),
           "S s3\nS s2\n S s2a\n S s2b\nS s1\nA 0+\n");
    expect(t, "sidebar.dragarchived", istr(&a, drag[5].archived*10 + drag[0].archived), "10");
    in.archive = 0;

    // A session with subagents folds, by its key
    *upsert(&in.folds, "s:this:s2", &a) = 1;
    Slice<SideRow> rows = sidetree(&a, &in);
    expect(t, "sidebar.foldsess", siderows(&a, &in, rows), "S s3\nS s2+\nS s1\n");
    expect(t, "sidebar.foldkey", rows[1].key, "s:this:s2");
    expect(t, "sidebar.kids", istr(&a, rows[1].kids*10 + rows[0].kids), "10");

    // A subagent at work keeps it open; one asking too
    in.sess[1].active = 1;
    expect(t, "sidebar.activeopen", siderows(&a, &in, sidetree(&a, &in)),
           "S s3\nS s2!\n S s2a\n S s2b\nS s1\n");
    in.sess[1].active = 0;
    in.sess[2].asks   = 1;
    expect(t, "sidebar.askopen", siderows(&a, &in, sidetree(&a, &in)),
           "S s3\nS s2!\n S s2a\n S s2b\nS s1\n");
    in.sess[2].asks = 0;
    // The session itself at work does not
    in.sess[3].active = 1;
    expect(t, "sidebar.selfactive", siderows(&a, &in, sidetree(&a, &in)),
           "S s3\nS s2+\nS s1\n");
    in.sess[3].active = 0;

    // Two hosts: headings, every host listed always even if empty, a
    // group merged across hosts, its subagents following their root,
    // and a placeholder group in the GUI's order before it
    SideIn two = {};
    two.heads  = 1;
    two.hosts  = push(&a, two.hosts, SideHost{"this", 1});
    two.hosts  = push(&a, two.hosts, SideHost{"remote:vm", 1});
    two.hosts  = push(&a, two.hosts, SideHost{"remote:pi", 1});
    two.hosts  = push(&a, two.hosts, SideHost{"remote:off", 0});
    two.groups = push(&a, two.groups, Str("empty"));
    two.sess   = push(&a, two.sess, sidesess(0, "a3", {}, "work", 0));
    two.sess   = push(&a, two.sess, sidesess(0, "a2", {}, {}, 0));
    two.sess   = push(&a, two.sess, sidesess(0, "a1k", "a1", "other", 0));
    two.sess   = push(&a, two.sess, sidesess(0, "a1", {}, "work", 0));
    two.sess   = push(&a, two.sess, sidesess(1, "b2", {}, "work", 0));
    two.sess   = push(&a, two.sess, sidesess(1, "b1", {}, {}, 1));
    two.sess   = push(&a, two.sess, sidesess(1, "b1k", "b1", {}, 0));
    two.sess   = push(&a, two.sess, sidesess(0, "a0", {}, "work", 1));
    expect(t, "sidebar.hosts", siderows(&a, &two, sidetree(&a, &two)),
           "G empty 0\n"
           "G work 4\n"
           " H this 3\n"
           "  S a3\n"
           "  S a1\n"
           "   S a1k\n"
           " H remote:vm 1\n"
           "  S b2\n"
           "H this 1\n"
           " S a2\n"
           "H remote:vm 0\n"
           "H remote:pi 0\n"
           "A 3+\n");

    // The Archived heading opened, by host; a group and a host folded
    *upsert(&two.folds, "a", &a)              = 0;
    *upsert(&two.folds, "g:work", &a)         = 1;
    *upsert(&two.folds, "h:remote:vm", &a)    = 1;
    expect(t, "sidebar.archived", siderows(&a, &two, sidetree(&a, &two)),
           "G empty 0\n"
           "G work 4+\n"
           "H this 1\n"
           " S a2\n"
           "H remote:vm 0+\n"
           "H remote:pi 0\n"
           "A 3\n"
           " H this 1\n"
           "  S a0\n"
           " H remote:vm 2\n"
           "  S b1\n"
           "   S b1k\n");

    // An ask inside opens the group, and the host heading under it, but
    // not the heading of a host it is not on
    two.sess[2].asks = 1;  // a1k
    *upsert(&two.folds, "g:work|h:this", &a)      = 1;
    *upsert(&two.folds, "g:work|h:remote:vm", &a) = 1;
    expect(t, "sidebar.askgroup", siderows(&a, &two, sidetree(&a, &two)),
           "G empty 0\n"
           "G work 4!\n"
           " H this 3!\n"
           "  S a3\n"
           "  S a1!\n"
           "   S a1k\n"
           " H remote:vm 1+\n"
           "H this 1\n"
           " S a2\n"
           "H remote:vm 0+\n"
           "H remote:pi 0\n"
           "A 3\n"
           " H this 1\n"
           "  S a0\n"
           " H remote:vm 2\n"
           "  S b1\n"
           "   S b1k\n");
    two.sess[2].asks = 0;

    // Subagents on another host than their parent's (phost), under it in
    // its group and heading, marked away; one whose parent is not here
    // a root on its own host; and the same sid on each host kept apart
    SideIn far = {};
    far.heads = 1;
    far.hosts = push(&a, far.hosts, SideHost{"this", 1});
    far.hosts = push(&a, far.hosts, SideHost{"remote:r", 1});
    far.sess  = push(&a, far.sess, sidesess(0, "p", {}, "work", 0));
    far.sess  = push(&a, far.sess, sidesess(1, "k2", "p", {}, 0));
    far.sess  = push(&a, far.sess, sidesess(1, "k1", "p", {}, 0));
    far.sess  = push(&a, far.sess, sidesess(1, "o", {}, {}, 0));
    far.sess  = push(&a, far.sess, sidesess(1, "p", {}, {}, 0));
    far.sess  = push(&a, far.sess, sidesess(1, "q", "p", {}, 0));
    far.sess[1].phost = 1;
    far.sess[2].phost = 1;
    Slice<SideRow> frows = sidetree(&a, &far);
    expect(t, "sidebar.away", siderows(&a, &far, frows),
           "G work 3\n"
           " H this 3\n"
           "  S p\n"
           "   S k1@\n"
           "   S k2@\n"
           "H this 0\n"
           "H remote:r 3\n"
           " S o\n"
           " S p\n"
           "  S q\n");
    expect(t, "sidebar.awayhost", istr(&a, frows[3].host), "1");

    // Folded under its parent, unless one asks
    *upsert(&far.folds, "s:this:p", &a) = 1;
    expect(t, "sidebar.awayfold", siderows(&a, &far, sidetree(&a, &far)),
           "G work 3\n"
           " H this 3\n"
           "  S p+\n"
           "H this 0\n"
           "H remote:r 3\n"
           " S o\n"
           " S p\n"
           "  S q\n");
    far.sess[1].asks = 1;
    expect(t, "sidebar.awayask", siderows(&a, &far, sidetree(&a, &far)),
           "G work 3!\n"
           " H this 3!\n"
           "  S p!\n"
           "   S k1@\n"
           "   S k2@\n"
           "H this 0\n"
           "H remote:r 3\n"
           " S o\n"
           " S p\n"
           "  S q\n");

    // Parents in a cycle still show, each once, after the roots (one
    // its own parent is one)
    SideIn cyc = {};
    cyc.hosts = push(&a, cyc.hosts, SideHost{"this", 1});
    cyc.sess  = push(&a, cyc.sess, sidesess(0, "x", "y", {}, 0));
    cyc.sess  = push(&a, cyc.sess, sidesess(0, "y", "x", {}, 0));
    cyc.sess  = push(&a, cyc.sess, sidesess(0, "z", "z", {}, 0));
    expect(t, "sidebar.cycle", siderows(&a, &cyc, sidetree(&a, &cyc)),
           "S z\nS x\n S y\n");

    // Nothing at all, one host: no rows; two hosts: their headings
    SideIn none = {};
    none.hosts = push(&a, none.hosts, SideHost{"this", 1});
    expect(t, "sidebar.none", siderows(&a, &none, sidetree(&a, &none)), "");
    none.heads = 1;
    none.hosts = push(&a, none.hosts, SideHost{"remote:vm", 1});
    expect(t, "sidebar.emptyhosts", siderows(&a, &none, sidetree(&a, &none)),
           "H this 0\nH remote:vm 0\n");
}

// An export as Markdown: the title and what the session is, messages
// under their authors, reasoning and tool calls only when asked for,
// and a call's output fenced longer than any run of backticks in it.
static void test_viewexport(Test *t)
{
    Arena a = t->perm;
    View *v = testview(&a);
    apply(v, a, R"({"sid":"s","seq":1,"ts":0,"ev":"session","cwd":"/w","model":"m","endpoint":"e"})");
    apply(v, a, R"({"sid":"s","seq":2,"ts":0,"ev":"title","text":"Fix it"})");
    apply(v, a, R"({"sid":"s","seq":3,"ts":0,"ev":"user","text":"go","via":"prompt"})");
    apply(v, a, R"({"sid":"s","seq":4,"ts":0,"ev":"turn","n":1})");
    apply(v, a, R"({"sid":"s","seq":5,"ts":0,"ev":"assistant","turn":1,"attempt":1,"blocks":[)"
                R"({"t":"think","text":"why\nso"},{"t":"text","text":"Looking."},)"
                R"({"t":"call","id":"c1","name":"shell","args":"{\"command\":\"ls\"}"}]})");
    apply(v, a, R"({"sid":"s","seq":6,"ts":0,"ev":"tool.start","call":"c1","name":"shell","args":"{}","cwd":"/w"})");
    apply(v, a, R"({"sid":"s","seq":7,"ts":0,"ev":"tool.end","call":"c1","status":"ok","exit":0,"text":"a ```b```\n","ms":5})");
    apply(v, a, R"({"sid":"s","seq":8,"ts":0,"ev":"assistant","turn":1,"attempt":2,"blocks":[{"t":"text","text":"Done."}]})");

    Buf b(&a, 1024);
    viewexport(&b, v, "s", 0, 0, 0);
    expect(t, "export.plain", finish(&b),
           "# Fix it\n\nSession s, m, in `/w`\n"
           "\n## You\n\ngo\n"
           "\n## Agent\n\nLooking.\n"
           "\n## Agent\n\nDone.\n");

    Buf c(&a, 1024);
    viewexport(&c, v, "s", 1, 1, 1);
    expect(t, "export.all", finish(&c),
           "# Fix it\n\nSession s, m, in `/w`\n"
           "\n## You\n\ngo\n"
           "\n## Agent\n\n> **Reasoning**\n>\n> why\n> so\n"
           "\nLooking.\n"
           "\n**Tool: shell** `ls`\n\n````\na ```b```\n````\n"
           "\n## Agent\n\nDone.\n");
}
