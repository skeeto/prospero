// Window placement tests: a saved window on displays as they are now,
// faked: kept where the displays cover it, fitted onto the display with
// most of its title bar, or centered on the primary when it was on a
// display gone since. With nothing saved, the first size at the
// display's scale, or maximized on a display too small for it.

static Str boxstr(Arena *a, WinBox b)
{
    Buf p(a, 64);
    print(&p, (i64)b.x);
    putbyte(&p, ',');
    print(&p, (i64)b.y);
    putbyte(&p, ' ');
    print(&p, (i64)b.w);
    putbyte(&p, 'x');
    print(&p, (i64)b.h);
    return finish(&p);
}

static void test_place(Test *t)
{
    Arena a = t->perm;

    // A laptop's display under a menu bar, and a second one to its right
    WinBox two[] = {{0, 25, 1440, 875}, {1440, 0, 1920, 1080}};
    WinBox one[] = {{0, 25, 1440, 875}};

    WinBox w = {100, 100, 1280, 800};
    expect(t, "place.kept", boxstr(&a, placewindow(w, one, 1)), "100,100 1280x800");
    expect(t, "place.nodisplays", boxstr(&a, placewindow(w, 0, 0)), "100,100 1280x800");

    // Across both displays, wholly on them, stays
    w = {1000, 100, 1200, 700};
    expect(t, "place.spans", boxstr(&a, placewindow(w, two, 2)), "1000,100 1200x700");

    // On the second display, now gone: centered on the primary, fitted
    w = {2000, 100, 1280, 800};
    expect(t, "place.second", boxstr(&a, placewindow(w, two, 2)), "2000,100 1280x800");
    expect(t, "place.gone", boxstr(&a, placewindow(w, one, 1)), "80,62 1280x800");
    w = {1600, 50, 1800, 1000};
    expect(t, "place.gonebig", boxstr(&a, placewindow(w, one, 1)), "0,25 1440x875");

    // Under the menu bar: moved down onto the display
    w = {100, 10, 800, 600};
    expect(t, "place.menubar", boxstr(&a, placewindow(w, one, 1)), "100,25 800x600");

    // Bigger than its display, after the resolution went down
    w = {50, 40, 2560, 1400};
    expect(t, "place.shrunk", boxstr(&a, placewindow(w, one, 1)), "0,25 1440x875");

    // Hanging off the right edge, its title bar partly on the display
    w = {1000, 200, 800, 600};
    expect(t, "place.right", boxstr(&a, placewindow(w, one, 1)), "640,200 800x600");

    // Its title bar above every display, though some of it shows
    w = {100, -300, 800, 600};
    expect(t, "place.notitle", boxstr(&a, placewindow(w, one, 1)), "320,162 800x600");

    // A gap below the shorter display: onto the one with the title bar
    WinBox tall[] = {{0, 0, 1000, 600}, {1000, 0, 1000, 1000}};
    w = {900, 100, 400, 800};
    expect(t, "place.gap", boxstr(&a, placewindow(w, tall, 2)), "1000,100 400x800");

    // Nonsense saved sizes come up to the minimum
    w = {10, 30, 0, -5};
    expect(t, "place.tiny", boxstr(&a, placewindow(w, one, 1)), "10,30 320x240");

    // Negative coordinates: a display left of the primary
    WinBox left[] = {{0, 0, 1920, 1040}, {-1280, 0, 1280, 1024}};
    w = {-1200, 50, 1000, 700};
    expect(t, "place.left", boxstr(&a, placewindow(w, left, 2)), "-1200,50 1000x700");
    w = {-1200, 50, 1000, 700};
    expect(t, "place.leftgone", boxstr(&a, placewindow(w, left, 1)), "460,170 1000x700");
}

static Str firststr(Arena *a, WinBox disp, f32 scale)
{
    WinGeom g = firstgeom(disp, scale);
    Buf     p(a, 64);
    print(&p, (i64)g.box.w);
    putbyte(&p, 'x');
    print(&p, (i64)g.box.h);
    print(&p, g.max ? Str(" max") : Str(""));
    print(&p, g.ok ? Str(" ok") : Str(""));
    return finish(&p);
}

static void test_firstgeom(Test *t)
{
    Arena a = t->perm;

    // A 1080p display under a task bar: room at scale 1, not at 1.25
    WinBox hd = {0, 0, 1920, 1040};
    expect(t, "first.hd", firststr(&a, hd, 1), "1700x900");
    expect(t, "first.hd125", firststr(&a, hd, 1.25f), "1440x780 max");

    // 4K at 200%, as pixels (Win32, X11): the same in logical units
    WinBox uhd = {0, 0, 3840, 2080};
    expect(t, "first.uhd", firststr(&a, uhd, 2), "3400x1800");

    // A laptop in points (macOS) under its menu bar and dock, and one a
    // little short of the title bar's room
    expect(t, "first.laptop", firststr(&a, {0, 38, 1728, 1009}, 1), "1700x900");
    expect(t, "first.short", firststr(&a, {0, 25, 1728, 920}, 1), "1296x690 max");

    // Narrower than the window: maximized, never below the minimum
    expect(t, "first.narrow", firststr(&a, {0, 25, 1440, 875}, 1), "1080x657 max");
    expect(t, "first.tiny", firststr(&a, {0, 0, 200, 100}, 1), "320x240 max");

    // An unknown display, or scale: the size as is
    expect(t, "first.nodisplay", firststr(&a, {}, 1.5f), "2550x1350");
    expect(t, "first.noscale", firststr(&a, hd, 0), "1700x900");
}
