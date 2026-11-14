// Drop tests: where a drop goes, and the words that say so.

// A verdict as one line: to, label, detail, amber and refuse
static Str dropsay(Arena *a, DropFacts f, DropLoad l)
{
    DropWhere w = dropwhere(a, f, l);
    Buf b(a, 256);
    print(&b, droptoname(w.to));
    print(&b, " | ");
    print(&b, w.label);
    print(&b, " | ");
    print(&b, w.detail);
    print(&b, w.amber  ? Str(" | amber")  : Str(""));
    print(&b, w.refuse ? Str(" | refuse") : Str(""));
    return finish(&b);
}

// A card's box as "x0,y0 x1,y1", whole numbers
static Str boxsay(Arena *a, DropBox b)
{
    Buf o(a, 64);
    print(&o, (i64)b.x0);
    putbyte(&o, ',');
    print(&o, (i64)b.y0);
    putbyte(&o, ' ');
    print(&o, (i64)b.x1);
    putbyte(&o, ',');
    print(&o, (i64)b.y1);
    return finish(&o);
}

static void test_cardplace(Test *t)
{
    Arena   a    = t->perm;
    DropBox work = {0, 0, 1000, 800};
    DropBox none = {};

    // Centered in a pane at least three cards tall
    expect(t, "cardplace.pane", boxsay(&a, cardplace({600, 100, 1000, 700}, 200, 40, work, 0, 0, 8)),
           "700,380 900,420");
    // Exactly three cards tall is tall enough
    expect(t, "cardplace.three", boxsay(&a, cardplace({0, 200, 400, 320}, 200, 40, work, 0, 0, 8)),
           "100,240 300,280");
    // Above a one-line field, centered over it
    expect(t, "cardplace.above", boxsay(&a, cardplace({300, 400, 700, 430}, 200, 40, work, 0, 0, 8)),
           "400,352 600,392");
    // Below a field at the top, with no room above
    expect(t, "cardplace.below", boxsay(&a, cardplace({300, 20, 700, 50}, 200, 40, work, 0, 0, 8)),
           "400,58 600,98");
    // No place: by the pointer
    expect(t, "cardplace.pointer", boxsay(&a, cardplace(none, 200, 40, work, 100, 100, 8)),
           "108,108 308,148");
    // An empty place is no place
    expect(t, "cardplace.empty", boxsay(&a, cardplace({300, 300, 300, 400}, 200, 40, work, 100, 100, 8)),
           "108,108 308,148");
    // Clamped at each edge
    expect(t, "cardplace.right", boxsay(&a, cardplace(none, 200, 40, work, 900, 100, 8)),
           "800,108 1000,148");
    expect(t, "cardplace.bottom", boxsay(&a, cardplace(none, 200, 40, work, 100, 790, 8)),
           "108,760 308,800");
    expect(t, "cardplace.left", boxsay(&a, cardplace({-300, 400, 50, 430}, 200, 40, work, 0, 0, 8)),
           "0,352 200,392");
    expect(t, "cardplace.top", boxsay(&a, cardplace(none, 200, 40, work, -50, -30, 8)),
           "0,0 200,40");
    // Wider than work: at its near edge
    expect(t, "cardplace.wide", boxsay(&a, cardplace(none, 1200, 40, work, 500, 100, 8)),
           "0,108 1200,148");
    // Work not at the origin (a menu bar above it)
    DropBox below = {0, 20, 1000, 800};
    expect(t, "cardplace.workoff", boxsay(&a, cardplace({300, 30, 700, 60}, 200, 40, below, 0, 0, 8)),
           "400,68 600,108");
}

// A load as "kind n dirs"
static Str loadsay(Arena *a, DropLoad l)
{
    static Str const kinds[] = {"unknown", "files", "text", "other"};
    Buf b(a, 64);
    print(&b, kinds[l.kind]);
    putbyte(&b, ' ');
    print(&b, (i64)l.n);
    putbyte(&b, ' ');
    print(&b, (i64)l.dirs);
    return finish(&b);
}

static void test_dropload(Test *t)
{
    Arena a = t->perm;
    expect(t, "dropload.files", loadsay(&a, dropload(3, 3, 3, 1, 0)), "files 3 1");
    // Past the scan, all files when all the scanned were
    expect(t, "dropload.past", loadsay(&a, dropload(40, DROPLOAD_SCAN, DROPLOAD_SCAN, 2, 0)),
           "files 40 2");
    // Items neither files nor text are left out, the rest unknown
    expect(t, "dropload.mixed", loadsay(&a, dropload(40, DROPLOAD_SCAN, 5, 0, 1)), "files 5 0");
    expect(t, "dropload.huge", loadsay(&a, dropload((i64)1<<40, 16, 16, 0, 0)),
           "files 2147483647 0");
    expect(t, "dropload.text", loadsay(&a, dropload(2, 2, 0, 0, 1)), "text 0 0");
    expect(t, "dropload.other", loadsay(&a, dropload(2, 2, 0, 0, 0)), "other 0 0");
    expect(t, "dropload.empty", loadsay(&a, dropload(0, 0, 0, 0, 0)), "other 0 0");
}

static void test_dropto(Test *t)
{
    Arena a = t->perm;

    DropFacts f = {};
    f.room      = 10;
    f.attachmax = 10;
    f.tabs      = 8;

    DropLoad text    = {DRAGLOAD_TEXT, 1, 0};
    DropLoad other   = {DRAGLOAD_OTHER, 0, 0};
    DropLoad unknown = {DRAGLOAD_UNKNOWN, 0, 0};
    DropLoad one     = {DRAGLOAD_FILES, 1, 0};
    DropLoad three   = {DRAGLOAD_FILES, 3, 0};

    // Text and anything else: refused, wherever
    DropFacts over = f;
    over.at        = 1;
    over.overfiles = 1;
    expect(t, "dropto.text", dropsay(&a, over, text),
           "none | Only files can be dropped here |  | amber | refuse");
    expect(t, "dropto.other", dropsay(&a, f, other),
           "none | Only files can be dropped here |  | amber | refuse");

    // Folders start a New session, Open file shown or not
    DropFacts ask = over;
    ask.askshown  = 1;
    expect(t, "dropto.folder", dropsay(&a, ask, {DRAGLOAD_FILES, 1, 1}),
           "newsession | New session | in the folder dropped");
    expect(t, "dropto.folders", dropsay(&a, f, {DRAGLOAD_FILES, 2, 2}),
           "newsession | New session | in the last of 2 folders");
    // Past what is scanned, the first DROPLOAD_SCAN decide
    expect(t, "dropto.scan", dropsay(&a, f, {DRAGLOAD_FILES, 20, DROPLOAD_SCAN}),
           "newsession | New session | in the last of 20 folders");

    // Open file takes the first, whatever else holds
    DropFacts askf = f;
    askf.askshown  = 1;
    expect(t, "dropto.field1", dropsay(&a, askf, one), "field | Fill in the path | ");
    expect(t, "dropto.field3", dropsay(&a, askf, three),
           "field | Fill in the path | the first of 3 files only");
    DropFacts askall = ask;
    askall.modal     = 1;
    askall.attachwhy = "Wait for the attachments being sent";
    expect(t, "dropto.fieldall", dropsay(&a, askall, three),
           "field | Fill in the path | the first of 3 files only");

    // A modal: attach behind it, refused as an attach is
    DropFacts modal = over;
    modal.modal     = 1;
    expect(t, "dropto.modal", dropsay(&a, modal, one),
           "attach | Attach | to the next message, behind this dialog");
    modal.attachwhy = "Open a session to attach files to its next message";
    expect(t, "dropto.modalwhy", dropsay(&a, modal, three),
           "attach | Not attached | Open a session to attach files to its next message"
           " | amber | refuse");

    // Files at a point, and more than it keeps
    expect(t, "dropto.files1", dropsay(&a, over, one),
           "files | Open in Files | to read, not attached");
    expect(t, "dropto.files9", dropsay(&a, over, {DRAGLOAD_FILES, 9, 0}),
           "files | Open 9 in Files | only the last 8 stay open");

    // Over Files with no point (the title bar): attached
    DropFacts nopoint = over;
    nopoint.at        = 0;
    expect(t, "dropto.nopoint", dropsay(&a, nopoint, three),
           "attach | Attach 3 | to the next message");

    // Room, all or nothing
    DropFacts room = f;
    room.room      = 2;
    expect(t, "dropto.room2", dropsay(&a, room, three),
           "attach | Not attached | Room for 2 more in this message | amber | refuse");
    room.room = 1;
    expect(t, "dropto.room1", dropsay(&a, room, one), "attach | Attach | to the next message");
    expect(t, "dropto.max", dropsay(&a, f, {DRAGLOAD_FILES, 12, 0}),
           "attach | Not attached | At most 10 files go in a message | amber | refuse");
    // The edges: exactly the most, one past it, exactly the room, none
    expect(t, "dropto.max10", dropsay(&a, f, {DRAGLOAD_FILES, 10, 0}),
           "attach | Attach 10 | to the next message");
    expect(t, "dropto.max11", dropsay(&a, f, {DRAGLOAD_FILES, 11, 0}),
           "attach | Not attached | At most 10 files go in a message | amber | refuse");
    room.room = 3;
    expect(t, "dropto.room3", dropsay(&a, room, three), "attach | Attach 3 | to the next message");
    expect(t, "dropto.none", dropsay(&a, f, {DRAGLOAD_FILES, 0, 0}),
           "attach | Attach | to the next message");

    // Not known: no counts, never refused to the platform
    expect(t, "dropto.unknownfiles", dropsay(&a, over, unknown),
           "files | Open in Files | to read, not attached");
    expect(t, "dropto.unknownprompt", dropsay(&a, f, unknown),
           "attach | Attach | to the next message");
    DropFacts why = f;
    why.attachwhy = "This model takes no images: [model] vision is off for it";
    why.room      = 0;
    expect(t, "dropto.unknownwhy", dropsay(&a, why, unknown),
           "attach | Not attached | This model takes no images: [model] vision is off for it"
           " | amber");

    // Folders with files: the files go as files would, the folder too
    DropLoad mixed = {DRAGLOAD_FILES, 3, 1};
    expect(t, "dropto.mixedfiles", dropsay(&a, over, mixed),
           "files | Open 2 in Files | to read, not attached, and New session for the folder");
    DropFacts full = f;
    full.room      = 1;
    expect(t, "dropto.mixedfull", dropsay(&a, full, mixed),
           "attach | Not attached | Room for 1 more in this message,"
           " and New session for the folder | amber");
    expect(t, "dropto.mixedfield", dropsay(&a, askf, {DRAGLOAD_FILES, 3, 2}),
           "field | Fill in the path | and New session for the last folder");

    // The names
    expect(t, "dropto.names", droptoname(DROPTO_ATTACH), "attach");
    expect(t, "dropto.badname", droptoname(DROPTO_COUNT), "");

    // Room: the count's limit, else the slots'
    expect(t, "dropto.roomcount", istr(&a, attachroom(10, 8, 8, 14)), "2");
    expect(t, "dropto.roomslots", istr(&a, attachroom(10, 4, 13, 14)), "1");
    expect(t, "dropto.roomnone", istr(&a, attachroom(10, 10, 12, 14)), "0");

    test_cardplace(t);
}
