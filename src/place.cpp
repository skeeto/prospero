// Window placement: where a window saved on displays that may have
// changed since goes now
//
// A window opens where it was, unless that is no longer wholly on the
// displays: a display unplugged, its resolution lowered, or the menu bar
// or task bar moved over it. Then it is fitted, no bigger than the
// display that has the most of its title bar, and moved onto it; or, if
// no display has any of its title bar, which it needs to be moved by
// hand, centered on the primary display. A window with nothing saved
// opens PLACE_FIRSTW by PLACE_FIRSTH at its display's scale, where the
// platform puts it, or maximized if that display is too small to
// hold it. Each platform gives its own coordinates (WinGeom,
// prospero.cpp), and the frame's outer rectangle here, the title bar
// along its top.
//
// This is free and unencumbered software released into the public domain.

enum : i32 {
    PLACE_TITLE  = 30,    // the title bar's height, roughly
    PLACE_MINW   = 320,   // the smallest window a saved size makes
    PLACE_MINH   = 240,
    PLACE_FIRSTW = 1700,  // the client area with nothing saved, at scale 1
    PLACE_FIRSTH = 900,
};

static i64 boxoverlap(WinBox a, WinBox b)
{
    i64 x0 = a.x>b.x ? a.x : b.x;
    i64 y0 = a.y>b.y ? a.y : b.y;
    i64 x1 = (i64)a.x+a.w < (i64)b.x+b.w ? (i64)a.x+a.w : (i64)b.x+b.w;
    i64 y1 = (i64)a.y+a.h < (i64)b.y+b.h ? (i64)a.y+a.h : (i64)b.y+b.h;
    return x1>x0 && y1>y0 ? (x1-x0)*(y1-y0) : 0;
}

static i32 clampi32(i32 v, i32 lo, i32 hi)
{
    return v<lo ? lo : v>hi ? hi : v;
}

// Where a window's frame goes on the displays (usable rectangles, the
// primary first, which never overlap): where it is, if they cover it
// whole; else as above. With no displays known it stays as it is.
static WinBox placewindow(WinBox win, WinBox *disp, i32 n)
{
    if (n <= 0) {
        return win;
    }
    win.w = win.w<PLACE_MINW ? PLACE_MINW : win.w;
    win.h = win.h<PLACE_MINH ? PLACE_MINH : win.h;

    i64 covered = 0;
    for (i32 i = 0; i < n; i++) {
        covered += boxoverlap(win, disp[i]);
    }
    if (covered == (i64)win.w*win.h) {
        return win;
    }

    WinBox title = {win.x, win.y, win.w, PLACE_TITLE};
    i32    best  = -1;
    i64    most  = 0;
    for (i32 i = 0; i < n; i++) {
        i64 a = boxoverlap(title, disp[i]);
        if (a > most) {
            best = i;
            most = a;
        }
    }

    WinBox d = disp[best<0 ? 0 : best];
    win.w = win.w<d.w ? win.w : d.w;
    win.h = win.h<d.h ? win.h : d.h;
    if (best < 0) {
        win.x = d.x + (d.w - win.w)/2;
        win.y = d.y + (d.h - win.h)/2;
    } else {
        win.x = clampi32(win.x, d.x, d.x + d.w - win.w);
        win.y = clampi32(win.y, d.y, d.y + d.h - win.h);
    }
    return win;
}

// The geometry a window opens at with nothing saved, on a display of
// that usable rectangle (empty: unknown) at a scale: its client area
// PLACE_FIRSTW by PLACE_FIRSTH, scaled, where the platform puts it (not
// ok). If the display cannot hold that under a title bar, maximized,
// its normal box, should it be restored, three quarters of the display
// each way: one that filled it would count as maximized already, and
// macOS's zoom would take it smaller.
static WinGeom firstgeom(WinBox disp, f32 scale)
{
    scale = scale>0 ? scale : 1;
    WinGeom r     = {};
    i32     title = (i32)((f32)PLACE_TITLE*scale + 0.5f);
    r.box.w = (i32)((f32)PLACE_FIRSTW*scale + 0.5f);
    r.box.h = (i32)((f32)PLACE_FIRSTH*scale + 0.5f);
    if (disp.w>0 && disp.h>0 && (r.box.w>disp.w || r.box.h+title>disp.h)) {
        r.max   = 1;
        r.box.w = disp.w - disp.w/4;
        r.box.h = disp.h - disp.h/4;
        r.box.w = r.box.w<PLACE_MINW ? PLACE_MINW : r.box.w;
        r.box.h = r.box.h<PLACE_MINH ? PLACE_MINH : r.box.h;
    }
    return r;
}
