// The macOS desktop helper's own tests (main_desktop_macos.cpp built
// with DK_SELFTEST): its pure parts, and every command that needs a
// permission failing without it, through the core as the harness sends
// them. Nothing here captures the screen or posts input: a permission
// is checked before either, and both are treated as missing.

struct MacTest {
    Arena *a;
    i32    run;
    i32    failed;
};

static void check(MacTest *t, Str name, Str got, Str want)
{
    t->run++;
    if (!(got == want)) {
        t->failed++;
        Buf b(t->a, 256 + got.len + want.len);
        print(&b, "FAIL ");
        print(&b, name);
        print(&b, "\n  got:  [");
        print(&b, got);
        print(&b, "]\n  want: [");
        print(&b, want);
        print(&b, "]\n");
        writeall(2, finish(&b));
    }
}

static Str num(Arena *a, i64 v)
{
    Buf b(a, 24);
    print(&b, v);
    return finish(&b);
}

static Str rectstr(Arena *a, DkRect r)
{
    Buf b(a, 64);
    print(&b, (i64)r.left);
    putbyte(&b, ',');
    print(&b, (i64)r.top);
    putbyte(&b, ',');
    print(&b, (i64)r.right);
    putbyte(&b, ',');
    print(&b, (i64)r.bottom);
    return finish(&b);
}

// One request through the serve loop's handler: "status|text".
static Str serve(Arena *a, Dk *dk, Str args)
{
    Slice<Str> v = {};
    for (Str rest = args; rest.len;) {
        Cut c = cut(rest, '|');
        v    = push(a, v, c.head);
        rest = c.tail;
    }
    iz len = 0;
    for (iz i = 0; i < v.len; i++) {
        len += v[i].len + 1;
    }
    u8 *p = allocbytes(a, len);
    iz  n = 0;
    for (iz i = 0; i < v.len; i++) {
        copybytes(p+n, v[i].data, v[i].len);
        n += v[i].len;
        p[n++] = 0;
    }
    Str reply = dkserveone(dk, a, {p, n});
    u32 tlen  = dkget32le(reply.data+5);
    Buf b(a, 16 + tlen);
    print(&b, (i64)reply[4]);
    putbyte(&b, '|');
    print(&b, Str{reply.data+9, (iz)tlen});
    return finish(&b);
}

static i32 dkselftest(Arena *a, Arena perm)
{
    MacTest t_ = {};
    MacTest *t = &t_;
    t->a = a;

    // Points: CoreGraphics' rectangles rounded to whole points, and back
    {
        CGRect r = CGRectMake(10.4, 20.6, 100.2, 50.5);
        check(t, "rectof", rectstr(a, rectof(r)), "10,21,111,71");
        check(t, "rectof negative", rectstr(a, rectof(CGRectMake(-1440.5, -900, 1440, 900))),
              "-1441,-900,-1,0");
        CGRect c = cgrect({-1440, 0, 0, 900});
        check(t, "cgrect", concat(a, num(a, (i64)c.origin.x), num(a, (i64)c.size.width)),
              "-14401440");
        check(t, "ptround huge", num(a, ptround(1e300)), num(a, 1<<30));
    }

    // Which windows are listed, and which are popups
    {
        DkRect r = {0, 0, 10, 10};
        check(t, "listed normal", num(a, listed(0, 1, r, "TextEdit")), "1");
        check(t, "listed menu", num(a, listed(LEVEL_POPUP, 1, r, "TextEdit")), "1");
        check(t, "listed menubar", num(a, listed(LEVEL_MAINMENU, 1, r, "Window Server")), "0");
        check(t, "listed status", num(a, listed(LEVEL_STATUS, 1, r, "Control Center")), "0");
        check(t, "listed dock", num(a, listed(LEVEL_DOCK, 1, r, "Dock")), "0");
        check(t, "listed desktop", num(a, listed(-2147483623, 1, r, "Finder")), "0");
        check(t, "listed clear", num(a, listed(0, 0, r, "TextEdit")), "0");
        check(t, "listed empty", num(a, listed(0, 1, {5, 5, 5, 9}, "TextEdit")), "0");
        check(t, "listed server", num(a, listed(0, 1, r, "Window Server")), "0");
        check(t, "listed saver", num(a, listed(LEVEL_TOP, 1, r, "ScreenSaverEngine")), "0");
        check(t, "popup normal", num(a, popuplevel(0)), "0");
        check(t, "popup floating", num(a, popuplevel(3)), "0");
        check(t, "popup menu", num(a, popuplevel(LEVEL_POPUP)), "1");
    }

    // Click counts: a double click is two presses close in time and place
    {
        Click c = {};
        check(t, "click first", num(a, clickcount(&c, 0, 100, 100, 1000)), "1");
        check(t, "click second", num(a, clickcount(&c, 0, 101, 99, 1070)), "2");
        check(t, "click third", num(a, clickcount(&c, 0, 101, 99, 1200)), "3");
        check(t, "click late", num(a, clickcount(&c, 0, 101, 99, 1200+MAC_DOUBLEMS+1)), "1");
        check(t, "click far", num(a, clickcount(&c, 0, 120, 99, 1800)), "1");
        check(t, "click other", num(a, clickcount(&c, 1, 120, 99, 1850)), "1");
    }

    // Keys, by name and by character on the US layout
    {
        i32 map[128];
        uskeymap(map);
        b32 shift = 0;
        check(t, "key cmd", num(a, keyfor(map, "cmd", &shift)), num(a, 0x37));
        check(t, "key win", num(a, keyfor(map, "Win", &shift)), num(a, 0x37));
        check(t, "key ctrl", num(a, keyfor(map, "ctrl", &shift)), num(a, 0x3B));
        check(t, "key delete", num(a, keyfor(map, "delete", &shift)), num(a, 0x33));
        check(t, "key del", num(a, keyfor(map, "del", &shift)), num(a, 0x75));
        check(t, "key enter", num(a, keyfor(map, "enter", &shift)), num(a, 0x24));
        check(t, "key f1", num(a, keyfor(map, "f1", &shift)), num(a, 0x7A));
        check(t, "key f12", num(a, keyfor(map, "F12", &shift)), num(a, 0x6F));
        check(t, "key f20", num(a, keyfor(map, "f20", &shift)), num(a, 0x5A));
        check(t, "key f21", num(a, keyfor(map, "f21", &shift)), "-1");
        check(t, "key unknown", num(a, keyfor(map, "zz", &shift)), "-1");
        check(t, "key w", num(a, keyfor(map, "w", &shift)*10 + shift), num(a, 0x0D*10));
        check(t, "key W", num(a, keyfor(map, "W", &shift)*10 + shift), num(a, 0x0D*10));
        check(t, "key !", num(a, keyfor(map, "!", &shift)*10 + shift), num(a, 0x12*10 + 1));
        check(t, "key ?", num(a, keyfor(map, "?", &shift)*10 + shift), num(a, 0x2C*10 + 1));
        check(t, "key plus", num(a, keyfor(map, "plus", &shift)*10 + shift), num(a, 0x18*10));
        check(t, "key comma", num(a, keyfor(map, "comma", &shift)), num(a, 0x2B));
        check(t, "key backtick", num(a, keyfor(map, "`", &shift)), num(a, 0x32));
        check(t, "key nonascii", num(a, keyfor(map, "\xc3\xa9", &shift)), "-1");
        check(t, "mod cmd", num(a, modflag(0x37)==kCGEventFlagMaskCommand), "1");
        check(t, "mod shift", num(a, modflag(0x38)==kCGEventFlagMaskShift), "1");
        check(t, "mod none", num(a, (i64)modflag(0x0D)), "0");

        // A chord through the core, as the key command parses it
        Dk dk = {};
        for (i32 i = 0; i < 128; i++) dk.keymap[i] = map[i];
        dk.keyready = 1;
        DkChord k = dkparsechord(&dk, "cmd+shift+z");
        check(t, "chord", concat(a, num(a, k.len), concat(a, ",", num(a, k.codes[2]))),
              concat(a, "3,", num(a, 0x06)));
        k = dkparsechord(&dk, "cmd+?");
        check(t, "chord implied shift", concat(a, num(a, k.len), concat(a, ",",
              num(a, k.codes[1]))), concat(a, "3,", num(a, 0x38)));
    }

    // Whether a window has the keyboard, from what Accessibility says,
    // and what the failure names when it has not. As in the first live
    // run: TextEdit active, its new window not key, so keys were lost.
    {
        DkWin w = {};
        w.id    = 0x2a;
        w.pid   = 500;
        w.title = "Untitled";
        w.cls   = "TextEdit";
        Focus f = {};
        f.pid   = 500;
        f.app   = "TextEdit";
        f.key   = -1;
        check(t, "kb no window", num(a, haskeyboard(&f, 500, 0x2a)), "0");
        check(t, "kb no window why", unfocused(a, &f, &w),
              "cannot give the keyboard to the window 'Untitled' of TextEdit (500): "
              "it is frontmost, but with no window focused; no input was sent");
        check(t, "kb no element", num(a, haskeyboard(&f, 500, 0)), "1");
        check(t, "kb no element other", num(a, haskeyboard(&f, 501, 0)), "0");

        f.win = 0x2a;
        check(t, "kb key unknown", num(a, haskeyboard(&f, 500, 0x2a)), "1");
        f.key = 1;
        check(t, "kb key", num(a, haskeyboard(&f, 500, 0x2a)), "1");
        f.key = 0;
        check(t, "kb not key", num(a, haskeyboard(&f, 500, 0x2a)), "0");
        check(t, "kb not key why", unfocused(a, &f, &w),
              "cannot give the keyboard to the window 'Untitled' of TextEdit (500): "
              "the window is focused, but not key; no input was sent");
        f.sheet = 1;
        check(t, "kb sheet shown", num(a, haskeyboard(&f, 500, 0x2a)), "1");
        f.sheet = 0;

        f.win   = 0x2b;
        f.title = "Untitled 2";
        f.key   = -1;
        check(t, "kb other window", num(a, haskeyboard(&f, 500, 0x2a)), "0");
        check(t, "kb other window why", unfocused(a, &f, &w),
              "cannot give the keyboard to the window 'Untitled' of TextEdit (500): "
              "it is frontmost, but its window 'Untitled 2' (0x0000002b) is focused; "
              "no input was sent");
        f.attached = 1;  // the sheet is the focused window
        check(t, "kb sheet focused", num(a, haskeyboard(&f, 500, 0x2a)), "1");
        f.attached = 0;

        f.pid   = 77;
        f.app   = "Terminal";
        f.win   = 0x10;
        f.title = "zsh";
        check(t, "kb other app", num(a, haskeyboard(&f, 500, 0x2a)), "0");
        check(t, "kb other app no element", num(a, haskeyboard(&f, 500, 0)), "0");
        check(t, "kb other app why", unfocused(a, &f, &w),
              "cannot give the keyboard to the window 'Untitled' of TextEdit (500): "
              "Terminal (77) is frontmost, its window 'zsh' focused; no input was sent");
        f.win = 0;
        f.app = {};
        check(t, "kb other app bare why", unfocused(a, &f, &w),
              "cannot give the keyboard to the window 'Untitled' of TextEdit (500): "
              "another application (77) is frontmost; no input was sent");
        f = {};
        check(t, "kb nothing", num(a, haskeyboard(&f, 0, 0)), "0");
        check(t, "kb nothing why", unfocused(a, &f, &w),
              "cannot give the keyboard to the window 'Untitled' of TextEdit (500): "
              "no application is frontmost; no input was sent");
    }

    // Who permissions are granted to, and the message when one is missing
    {
        Str exe = "/Users/u/src/harness/build-signed/Prospero.app/Contents/MacOS/prospero";
        check(t, "appof", appof(exe), "/Users/u/src/harness/build-signed/Prospero.app");
        check(t, "appof nested", appof("/Applications/A.app/Contents/Frameworks/"
              "H.app/Contents/MacOS/H"), "/Applications/A.app/Contents/Frameworks/H.app");
        check(t, "appof bare", appof("/usr/sbin/sshd"), "/usr/sbin/sshd");
        check(t, "appof lookalike", appof("/x/Foo.apple/bin/foo"), "/x/Foo.apple/bin/foo");
        check(t, "appof itself", appof("/Applications/Foo.app"), "/Applications/Foo.app");
        check(t, "grantee", grantee(a, exe),
              "Prospero.app (/Users/u/src/harness/build-signed/Prospero.app)");
        check(t, "grantee unknown", grantee(a, ""), "Prospero.app");
        check(t, "grantee bare", grantee(a, "/usr/sbin/sshd"), "/usr/sbin/sshd");
        Str helper = "/A/Prospero.app/Contents/MacOS/prospero-desktop";
        check(t, "grantee self", grantee(a, helper, 1), helper);
        check(t, "permmsg screen", permmsg(a, PERM_SCREEN, "Prospero.app (/A/Prospero.app)"),
              "Screen Recording permission is missing: grant it to Prospero.app "
              "(/A/Prospero.app) in System Settings > Privacy & Security > Screen "
              "Recording, then quit and reopen it");
        check(t, "permmsg ax", permmsg(a, PERM_AX, "Prospero.app"),
              "Accessibility permission is missing: grant it to Prospero.app in "
              "System Settings > Privacy & Security > Accessibility");
        check(t, "deny", num(a, parsedeny("screen, accessibility")), num(a, PERM_SCREEN|PERM_AX));
        check(t, "deny one", num(a, parsedeny("accessibility")), num(a, PERM_AX));
        check(t, "deny junk", num(a, parsedeny("x,,")), "0");
    }

    // Where run looks for an application
    {
        Slice<Str> p = appplaces(a, "TextEdit", "/Users/u");
        check(t, "places", concat(a, num(a, p.len), concat(a, " ", p[0])),
              "6 /Applications/TextEdit.app");
        check(t, "places system", p[1], "/System/Applications/TextEdit.app");
        check(t, "places home", p[5], "/Users/u/Applications/TextEdit.app");
        p = appplaces(a, "Safari.app", "");
        check(t, "places suffix", concat(a, num(a, p.len), concat(a, " ", p[0])),
              "5 /Applications/Safari.app");
        p = appplaces(a, "/Applications/Safari.app/", "");
        check(t, "places path", concat(a, num(a, p.len), concat(a, " ", p[0])),
              "1 /Applications/Safari.app");
        p = appplaces(a, "/usr/bin/true", "");
        check(t, "places program", num(a, p.len), "0");
        p = appplaces(a, "", "");
        check(t, "places empty", num(a, p.len), "0");
    }

    // Every command that needs a permission fails without it, naming it
    // and what to grant it to, before any capture or input
    {
        Dk dk = {};
        dk.perm    = perm;
        dk.held    = -1;
        dk.deny    = PERM_SCREEN | PERM_AX;
        dk.granted = "Prospero.app (/A/Prospero.app)";
        Str screen = concat(a, "1|", permmsg(a, PERM_SCREEN, dk.granted));
        Str ax     = concat(a, "1|", permmsg(a, PERM_AX, dk.granted));
        check(t, "hello", serve(a, &dk, "hello"), "0|prospero-desktop 1 macos points\n");
        check(t, "shot", serve(a, &dk, "shot|screen"), screen);
        check(t, "shot part", serve(a, &dk, "shot|-s|100|0|screen|0|0|10|10"), screen);
        check(t, "click", serve(a, &dk, "click|screen|1|1"), ax);
        check(t, "dclick", serve(a, &dk, "dclick|screen|1|1"), ax);
        check(t, "rclick", serve(a, &dk, "rclick|screen|1|1"), ax);
        check(t, "hover", serve(a, &dk, "hover|screen|1|1"), ax);
        check(t, "drag", serve(a, &dk, "drag|screen|1|1|5|5"), ax);
        check(t, "wheel", serve(a, &dk, "wheel|screen|1|1|-3"), ax);
        check(t, "key", serve(a, &dk, "key|screen|cmd+w"), ax);
        check(t, "type", serve(a, &dk, "type|screen|hi"), ax);
        check(t, "held", num(a, dk.held), "-1");
        check(t, "why taken", num(a, dk.why.data!=0), "0");
        check(t, "access", serve(a, &dk, "access"),
              "0|screen-recording\tmissing\naccessibility\tmissing\n"
              "granted-to\tProspero.app (/A/Prospero.app)\n");
        check(t, "access ask", serve(a, &dk, "access|-ask"),
              "0|screen-recording\tmissing\naccessibility\tmissing\n"
              "granted-to\tProspero.app (/A/Prospero.app)\n");

        // Window operations, on a window of no one's: without the
        // permission, it is the permission that is missing
        DkWin w = {};
        w.id  = 0xfffffff0;
        w.pid = 0x7ffffff0;
        check(t, "activate", num(a, dk_activate(&dk, &w)), "0");
        check(t, "activate why", dk_why(&dk), cuthead(ax, 2));
        check(t, "close", num(a, dk_close(&dk, &w)), "0");
        check(t, "close why", dk_why(&dk), cuthead(ax, 2));
        check(t, "move", num(a, dk_move(&dk, &w, 1, 2, 3, 4, 1)), "0");
        check(t, "move why", dk_why(&dk), cuthead(ax, 2));
        check(t, "close screen", serve(a, &dk, "close|screen"), "1|cannot close the screen");
        check(t, "no window", serve(a, &dk, "raise|hwnd=0xfffffff0"),
              "2|no window matches 'hwnd=0xfffffff0'");
        check(t, "units", dk_units(&dk), "points");
    }

    Buf b(a, 64);
    print(&b, (i64)t->run);
    print(&b, " tests, ");
    print(&b, (i64)t->failed);
    print(&b, " failed\n");
    writeall(t->failed ? 2 : 1, finish(&b));
    return t->failed != 0;
}

int main()
{
    Region *r  = reserve((iz)1 << 28);
    Region *pr = reserve((iz)1 << 24);
    if (!r || !pr) {
        os_oom();
    }
    Arena a = newarena(r);
    return dkselftest(&a, newarena(pr));
}
