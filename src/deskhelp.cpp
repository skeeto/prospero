// Desktop helper: prospero-desktop, which sees and drives a desktop
//
// A process of its own, apart from the harness, which the turn can stop
// and whose failures stay its own. On Windows, and in a Linux build that
// is not static, it is prospero itself, run as "prospero desktop".
// Elsewhere it is a program of its own too: on macOS so that the one
// executable with the operating system's grant to watch the screen and
// send input (macOS asks for it by program) keeps its identity, and
// beside a static Linux engine because it loads X11 and the portal's
// libraries, which a static program cannot. It started as winctl (the
// user's sshd, ~/src/sshd), whose command set it keeps:
//
//   list [-a] [WINDOW]              visible top-level windows (-a: all)
//   shot [-s EDGE PIXELS] [WINDOW [X Y W H]]
//                                   a PNG of the screen under WINDOW,
//                                   scaled to fit EDGE and PIXELS (0: any)
//   wait [-gone|-change|-idle] [-t SECS] WINDOW
//   click|dclick|rclick|hover WINDOW X Y
//   drag WINDOW X0 Y0 X1 Y1
//   wheel WINDOW X Y CLICKS         positive scrolls up
//   key WINDOW CHORD...             e.g. ctrl+s alt+f shift+tab enter
//   type WINDOW TEXT                TEXT as Unicode characters
//   raise WINDOW                    bring to the foreground
//   close WINDOW                    ask to close, like the title bar's X
//   move WINDOW X Y [W H]           move and resize, in screen units
//   run PROGRAM [ARGS...]           start detached, print the pid
//   access [-ask]                   the permissions it needs, if any
//
// WINDOW is a title substring, or key=value[,key=value...] with title=,
// class=, pid= and hwnd= (a window's id), or "screen", or "active" (the
// foreground window). Coordinates are relative to the window's visible
// frame, as in its shot; "screen" is relative to the top-left of the
// whole (virtual) screen. Their unit is the desktop's own, which hello
// names: physical pixels on Windows and X11, points on macOS, where a
// Retina display has two or more pixels to a point. A shot is captured
// at the display's full resolution and scaled down to the units at
// most, so that a pixel of the image is never smaller than a unit.
//
// run starts a program apart from the helper, and from the harness's
// reaper, so that it outlives both, as an application the user started
// does; where it cannot, it says so (in serve mode, on the line after
// the pid), since the program then ends with the harness.
//
// Run with a command, it does that one and exits, as winctl does: text
// and PNGs on standard output, errors on standard error, exit status 0
// (ok), 1 (error), 2 (no window or several match) or 3 (wait timed out).
// --version prints "prospero-desktop VERSION", touching no desktop.
// Run with --serve, it takes commands from the harness on standard input
// until its end, as frames: a little-endian u32 length, then the
// arguments, each ending in a null byte. Each gets one reply frame: a
// u32 length, then a status byte (the exit status above), a u32 text
// length, the text (output, or the error), and binary data (a PNG). The
// first command is hello, whose text is "prospero-desktop PROTO OS
// UNITS", the units being "pixels" or "points". In serve mode a shot's
// text is "OX OY W H IW IH ID": where the capture starts relative to the
// window's origin, the size captured, in units, the size of the image,
// never larger, which -s may have scaled down further, and the window's
// id in hex (0x0 for the screen), with which later commands can name it.
//
// Everything here is portable; the platform layer (main_desktop_*.cpp)
// implements the dk_* functions below for its desktop.
//
// This is free and unencumbered software released into the public domain.

// How it names itself in messages: a program of its own, or a command
// of prospero's on Windows, where the harness holds it (main_win32.cpp)
#ifndef DK_NAME
#  define DK_NAME "prospero-desktop"
#endif

enum : i32 {
    DK_OK      = 0,
    DK_FAIL    = 1,  // bad usage, or an operation failed
    DK_NOMATCH = 2,  // no window matches, or more than one
    DK_TIMEOUT = 3,  // wait ran out of time
};

enum : iz {
    DK_PROTO    = 1,
    DK_REQMAX   = 1 << 20,   // longest request frame
    DK_WAITMAX  = 600000,    // longest wait, milliseconds
};

struct DkRect {
    i32 left, top, right, bottom;
};

struct DkWin {
    u64    id;     // the platform's handle, 0 for the whole screen
    u32    pid;
    DkRect rect;   // its visible frame on the screen, in its units
    Str    title;  // UTF-8
    Str    cls;
    i32    level;  // the platform's stacking level, for dk_popup
};

struct DkSize {
    i32 w, h;
};


// Platform API (main_desktop_*.cpp)

struct Dk;

// The operating system, for hello: "windows", "macos", "linux".
static Str        dk_os(Dk *);
// The unit of every coordinate, for hello: "pixels" or "points".
static Str        dk_units(Dk *);
// Why the latest call that failed did, if the platform knows better
// than the generic message (a permission it lacks); else null.
static Str        dk_why(Dk *);
// The permissions the desktop needs, one line each, "NAME\tgranted" or
// "NAME\tmissing", plus what they are granted to; with ask, those
// missing are asked of the user first. 0 if any is still missing.
static b32        dk_access(Dk *, Buf *, b32 ask);
// Why the desktop cannot be used, such as no display to connect to, or
// null if it can. Asked before every command but help and hello.
static Str        dk_ready(Dk *);
// Top-level windows, top of the z-order first: those on the screen
// (visible and not cloaked, or minimized on Windows and X11), or with
// all, every one.
static Slice<DkWin> dk_windows(Dk *, Arena *, b32 all);
// One window by its id, with id 0 if there is no such window.
static DkWin      dk_window(Dk *, Arena *, u64 id);
// The whole (virtual) screen, with id 0.
static DkWin      dk_screen(Dk *);
// The foreground window's id, 0 if none.
static u64        dk_foreground(Dk *);
// Windows that selections skip unless they name them: popup menus,
// their shadows, and tooltips, which belong to the program under them.
static b32        dk_popup(Dk *, DkWin *);
// Bring a window to the foreground, restoring it if minimized, and
// update its rectangle. 0 if the system refused.
static b32        dk_activate(Dk *, DkWin *);
// The screen's pixels in a rectangle of units as 32-bit BGRX, top-down
// rows, at the display's resolution, whose size goes in size; null if
// the screen cannot be read (a locked desktop, no permission).
static u8        *dk_capture(Dk *, Arena *, DkRect, DkSize *size);
// Pointer input at a screen position in units; button 0 is
// the left, 1 the right; notches positive scroll up.
static b32        dk_pointer(Dk *, i32 x, i32 y);
static b32        dk_button(Dk *, i32 button, b32 down);
static b32        dk_wheel(Dk *, i32 notches);
// Pixels the pointer must travel with a button held to start a drag.
static i32        dk_dragstep(Dk *);
// The code of a key named in a chord (a modifier, a named key, or one
// character), and whether it needs shift; -1 if there is no such key.
static i32        dk_keycode(Dk *, Str name, b32 *shift);
// Press keys in order, then release them in reverse.
static b32        dk_keys(Dk *, i32 *codes, i32 n);
// Type text as Unicode characters, newlines and tabs as their keys.
static b32        dk_text(Dk *, Arena, Str);
// Ask a window to close, as its close button does.
static b32        dk_close(Dk *, DkWin *);
// Move a window's visible frame to x, y, and resize it with size.
static b32        dk_move(Dk *, DkWin *, i32 x, i32 y, i32 w, i32 h, b32 size);
// Start a program detached from this one, outside its job if it may;
// the pid, 0 on failure. attached: it could not leave the job.
static u32        dk_run(Dk *, Arena, Slice<Str> argv, b32 *attached);
// Wait for a process to wait for input: 0 when it does, 1 at the
// timeout, 2 if it cannot be waited for.
static i32        dk_idle(Dk *, u32 pid, i32 ms);
static void       dk_sleep(Dk *, i32 ms);
static i64        dk_ms(Dk *);  // monotonic milliseconds
// Standard input: bytes read, 0 at its end, -1 on error.
static iz         dk_read(Dk *, u8 *, iz);
// Standard output (1) or error (2), all of it.
static b32        dk_write(Dk *, i32 fd, Str);
// Whether standard output is a console, which a PNG must not go to.
static b32        dk_console(Dk *, i32 fd);


// Commands

struct DkCtx {
    Dk    *dk;
    Arena *a;        // the command's: reset after each
    Buf    out;      // text output
    Str    data;     // binary output, a PNG
    i32    status;
    Buf    err;      // the first failure's message
    b32    serving;
};

struct DkArgs {
    Str *v;
    i32  n;
    i32  i;  // next unconsumed
};

// Fail with a message, unless already failed: the first one stands.
static void dkfail(DkCtx *c, i32 status, Str msg, Str detail = {})
{
    if (c->status) {
        return;
    }
    c->status = status;
    print(&c->err, msg);
    if (detail.data) {
        print(&c->err, " '");
        print(&c->err, detail);
        print(&c->err, "'");
    }
}

static b32 dkmore(DkArgs *a)
{
    return a->i < a->n;
}

static Str dknext(DkCtx *c, DkArgs *a, Str what)
{
    if (!dkmore(a)) {
        dkfail(c, DK_FAIL, "missing argument:", what);
        return {};
    }
    return a->v[a->i++];
}

// A decimal integer, optionally negative, or 0x-prefixed hex.
static Int dkparseint(Str s)
{
    Int r = {};
    if (s.len>2 && s[0]=='0' && lowercase(s[1])=='x') {
        if (s.len > 18) {
            return r;
        }
        u64 v = 0;
        for (iz i = 2; i < s.len; i++) {
            u8  ch = lowercase(s[i]);
            i32 d  = digit(ch) ? ch-'0' : ch>='a' && ch<='f' ? ch-'a'+10 : -1;
            if (d < 0) {
                return r;
            }
            v = v<<4 | (u64)d;
        }
        r.ok  = v <= (u64)I64MAX;
        r.val = r.ok ? (i64)v : 0;
        return r;
    }
    return parseint(s);
}

static i32 dknextint(DkCtx *c, DkArgs *a, Str what)
{
    Str s = dknext(c, a, what);
    if (c->status) {
        return 0;
    }
    Int v = dkparseint(s);
    if (!v.ok || v.val<-0x7fffffff || v.val>0x7fffffff) {
        dkfail(c, DK_FAIL, "invalid number", s);
        return 0;
    }
    return (i32)v.val;
}

static void dkdone(DkCtx *c, DkArgs *a)
{
    if (dkmore(a)) {
        dkfail(c, DK_FAIL, "unexpected argument", a->v[a->i]);
    }
}

// Seconds with an optional fraction, as milliseconds; -1 if invalid.
static i64 dkparsems(Str s)
{
    Cut c     = cut(s, '.');
    Int whole = parseint(c.head);
    if (!whole.ok || whole.val<0 || whole.val>1000000 || (c.ok && !c.tail.len)) {
        return -1;
    }
    i64 ms    = whole.val * 1000;
    i64 scale = 100;
    for (iz i = 0; i < c.tail.len; i++, scale /= 10) {
        if (!digit(c.tail[i])) {
            return -1;
        }
        ms += (c.tail[i] - '0') * scale;
    }
    return ms;
}


// Choosing windows

struct DkSel {
    Str title;   // substring, ASCII case-insensitive
    Str cls;     // exact, ASCII case-insensitive
    u32 pid;
    u64 id;
    b32 screen;
    b32 active;
};

static b32 dkcontains(Str s, Str pat)
{
    for (iz i = 0; i+pat.len <= s.len; i++) {
        if (equalfold(Str{s.data+i, pat.len}, pat)) {
            return 1;
        }
    }
    return 0;
}

// "screen", "active", key=value criteria joined by commas (title=,
// class=, pid=, hwnd=), or else a title substring.
static DkSel dkparsesel(Str s)
{
    DkSel r = {};
    if (s == "screen") {
        r.screen = 1;
        return r;
    } else if (s == "active") {
        r.active = 1;
        return r;
    }

    DkSel t  = {};
    b32   ok = s.len > 0;
    for (Str rest = s; ok && rest.len;) {
        Cut c = cut(rest, ',');
        rest = c.tail;
        Str p = c.head;
        if (startswith(p, "title=")) {
            t.title = cuthead(p, 6);
        } else if (startswith(p, "class=")) {
            t.cls = cuthead(p, 6);
        } else if (startswith(p, "pid=")) {
            Int v = dkparseint(cuthead(p, 4));
            ok    = v.ok && v.val>0 && v.val<=0xffffffff;
            t.pid = (u32)v.val;
        } else if (startswith(p, "hwnd=")) {
            Int v = dkparseint(cuthead(p, 5));
            ok    = v.ok && v.val>0;
            t.id  = (u64)v.val;
        } else {
            ok = 0;
        }
    }
    if (ok) {
        return t;
    }
    r.title = s;
    return r;
}

static b32 dkmatches(DkSel *s, DkWin *w)
{
    return (!s->id         || s->id==w->id) &&
           (!s->pid        || s->pid==w->pid) &&
           (!s->cls.data   || equalfold(s->cls, w->cls)) &&
           (!s->title.data || dkcontains(w->title, s->title));
}

static Slice<DkWin> dkfindall(DkCtx *c, DkSel *s)
{
    Slice<DkWin> all = dk_windows(c->dk, c->a, s->id != 0);
    Slice<DkWin> r   = {};
    for (iz i = 0; i < all.len; i++) {
        b32 skip = !s->cls.data && !s->id && dk_popup(c->dk, &all[i]);
        if (!skip && dkmatches(s, &all[i])) {
            r = push(c->a, r, all[i]);
        }
    }
    return r;
}

static void dkprintwin(Buf *b, DkWin *w)
{
    print(b, "0x");
    printhex(b, w->id, 8);
    putbyte(b, '\t');
    print(b, (i64)w->pid);
    putbyte(b, '\t');
    print(b, (i64)w->rect.left);
    putbyte(b, ',');
    print(b, (i64)w->rect.top);
    putbyte(b, '\t');
    print(b, (i64)(w->rect.right - w->rect.left));
    putbyte(b, 'x');
    print(b, (i64)(w->rect.bottom - w->rect.top));
    putbyte(b, '\t');
    print(b, w->cls);
    putbyte(b, '\t');
    print(b, w->title);
    putbyte(b, '\n');
}

// Exactly one window, or fail with DK_NOMATCH.
static DkWin dkfindone(DkCtx *c, Str spec)
{
    DkWin r = {};
    if (c->status) {
        return r;
    }
    DkSel s = dkparsesel(spec);
    if (s.screen) {
        return dk_screen(c->dk);
    } else if (s.active) {
        u64 id = dk_foreground(c->dk);
        r = id ? dk_window(c->dk, c->a, id) : r;
        if (!r.id) {
            dkfail(c, DK_NOMATCH, "no active window");
        }
        return r;
    }

    Slice<DkWin> m = dkfindall(c, &s);
    if (!m.len) {
        // A desktop that cannot list some windows says why
        Str why = dk_why(c->dk);
        dkfail(c, DK_NOMATCH, "no window matches", spec);
        if (why.data) {
            print(&c->err, ": ");
            print(&c->err, why);
        }
    } else if (m.len > 1) {
        Buf b(c->a, 256);
        print(&b, (i64)m.len);
        print(&b, " windows match '");
        print(&b, spec);
        print(&b, "', narrow it down (e.g. hwnd=):\n");
        for (iz i = 0; i < m.len; i++) {
            dkprintwin(&b, &m[i]);
        }
        b.len--;  // the last newline
        dkfail(c, DK_NOMATCH, finish(&b));
    } else {
        r = m[0];
    }
    return r;
}

// Fail with the platform's reason, if it has one, else msg.
static void dkfailwhy(DkCtx *c, Str msg, Str detail = {})
{
    Str why = dk_why(c->dk);
    if (why.data) {
        dkfail(c, DK_FAIL, why);
    } else {
        dkfail(c, DK_FAIL, msg, detail);
    }
}

static void dkactivate(DkCtx *c, DkWin *w)
{
    if (!c->status && w->id && !dk_activate(c->dk, w)) {
        dkfailwhy(c, "cannot bring window to the foreground", w->title);
    }
}

static void dkinput(DkCtx *c, b32 ok)
{
    if (!ok) {
        // Blocked by UIPI (an elevated target), or a locked desktop
        dkfailwhy(c, "input failed (elevated window or locked desktop?)");
    }
}


// Scaling and PNG

// The largest size within edge on each side and pixels in all, keeping
// the aspect ratio, never larger than w by h. Zero limits are none.
static DkSize dkfit(i32 w, i32 h, i32 edge, i64 pixels)
{
    DkSize r = {w, h};
    f64    s = 1;
    i32    m = w>h ? w : h;
    if (edge>0 && m>edge) {
        s = (f64)edge / m;
    }
    if (pixels>0 && (f64)w*h*s*s > (f64)pixels) {
        // A square root by Newton's method, which needs no libm
        f64 x = (f64)pixels / ((f64)w*h);  // below 1
        f64 t = 1;
        for (i32 i = 0; i < 64; i++) {
            t = (t + x/t) / 2;
        }
        s = t<s ? t : s;
    }
    if (s < 1) {
        r.w = (i32)((f64)w*s);
        r.h = (i32)((f64)h*s);
        r.w = r.w<1 ? 1 : r.w;
        r.h = r.h<1 ? 1 : r.h;
    }
    return r;
}

// Scale 32-bit pixels down to w by h by averaging boxes: each output
// pixel is the mean of the source pixels mapping to it.
static u8 *dkscale(Arena *a, u8 *src, i32 sw, i32 sh, i32 w, i32 h)
{
    if (w==sw && h==sh) {
        return src;
    }
    u8 *dst = allocbytes(a, (iz)w*h*4);
    for (i32 y = 0; y < h; y++) {
        i32 y0 = (i32)((i64)y*sh/h);
        i32 y1 = (i32)((i64)(y+1)*sh/h);
        y1 = y1>y0 ? y1 : y0+1;
        for (i32 x = 0; x < w; x++) {
            i32 x0 = (i32)((i64)x*sw/w);
            i32 x1 = (i32)((i64)(x+1)*sw/w);
            x1 = x1>x0 ? x1 : x0+1;
            u32 sum[3] = {};
            for (i32 sy = y0; sy < y1; sy++) {
                u8 *row = src + ((iz)sy*sw + x0)*4;
                for (i32 sx = x0; sx < x1; sx++, row += 4) {
                    sum[0] += row[0];
                    sum[1] += row[1];
                    sum[2] += row[2];
                }
            }
            u32 n = (u32)((y1-y0) * (x1-x0));
            u8 *p = dst + ((iz)y*w + x)*4;
            p[0] = (u8)((sum[0] + n/2) / n);
            p[1] = (u8)((sum[1] + n/2) / n);
            p[2] = (u8)((sum[2] + n/2) / n);
            p[3] = 0;
        }
    }
    return dst;
}

// Encode 32-bit BGRX top-down pixels as a 24-bit RGB PNG (png.cpp).
static Str dkpng(Arena *a, u8 *bgrx, i32 w, i32 h)
{
    return pngencodez(a, bgrx, w, h, 1);
}

// The commands themselves

static void dkcmdlist(DkCtx *c, DkArgs *a)
{
    b32 all = 0;
    if (dkmore(a) && a->v[a->i]=="-a") {
        all = 1;
        a->i++;
    }
    DkSel s = {};
    if (dkmore(a)) {
        s = dkparsesel(dknext(c, a, "window"));
    }
    dkdone(c, a);
    if (c->status) {
        return;
    }
    Slice<DkWin> ws = dk_windows(c->dk, c->a, all || s.id);
    for (iz i = 0; i < ws.len; i++) {
        if (dkmatches(&s, &ws[i])) {
            dkprintwin(&c->out, &ws[i]);
        }
    }
    if (!ws.len) {
        // Nothing listed, since the desktop would not say
        Str why = dk_why(c->dk);
        if (why.data) {
            dkfail(c, DK_FAIL, why);
        }
    }
}

static void dkcmdshot(DkCtx *c, DkArgs *a)
{
    i32 edge   = 0;
    i32 pixels = 0;
    if (dkmore(a) && a->v[a->i]=="-s") {
        a->i++;
        edge   = dknextint(c, a, "edge");
        pixels = dknextint(c, a, "pixels");
        if (!c->status && (edge<0 || pixels<0)) {
            dkfail(c, DK_FAIL, "scale limits must not be negative");
        }
    }
    DkWin  w = dkfindone(c, dkmore(a) ? dknext(c, a, "window") : Str("screen"));
    DkRect r = w.rect;
    if (dkmore(a)) {
        // An optional sub-rectangle in window coordinates
        i32 x  = dknextint(c, a, "x");
        i32 y  = dknextint(c, a, "y");
        i32 cw = dknextint(c, a, "width");
        i32 ch = dknextint(c, a, "height");
        r.left  += x;
        r.top   += y;
        r.right  = r.left + cw;
        r.bottom = r.top  + ch;
    }
    dkdone(c, a);
    if (c->status) {
        return;
    }

    // Clip to the virtual screen, outside of which there is nothing
    DkRect v = dk_screen(c->dk).rect;
    r.left   = r.left   > v.left   ? r.left   : v.left;
    r.top    = r.top    > v.top    ? r.top    : v.top;
    r.right  = r.right  < v.right  ? r.right  : v.right;
    r.bottom = r.bottom < v.bottom ? r.bottom : v.bottom;
    i32 width  = r.right - r.left;
    i32 height = r.bottom - r.top;
    if (width<=0 || height<=0) {
        // or the screen itself cannot be seen, which the platform says
        dkfailwhy(c, "window is off screen or empty", w.title);
        return;
    } else if (!c->serving && dk_console(c->dk, 1)) {
        dkfail(c, DK_FAIL, "refusing to write PNG to a console, redirect it");
        return;
    }

    // Captured at the display's resolution, which on a Retina display
    // is a multiple of the units, then scaled straight to the image,
    // which is never larger than the units: points map back exactly
    DkSize px   = {};
    u8    *bits = dk_capture(c->dk, c->a, r, &px);
    if (!bits) {
        dkfailwhy(c, "screen capture failed (locked desktop?)");
        return;
    }
    DkSize s = dkfit(width, height, edge, pixels);
    bits    = dkscale(c->a, bits, px.w, px.h, s.w, s.h);
    c->data = dkpng(c->a, bits, s.w, s.h);
    if (c->serving) {
        print(&c->out, (i64)(r.left - w.rect.left));
        putbyte(&c->out, ' ');
        print(&c->out, (i64)(r.top - w.rect.top));
        putbyte(&c->out, ' ');
        print(&c->out, (i64)width);
        putbyte(&c->out, ' ');
        print(&c->out, (i64)height);
        putbyte(&c->out, ' ');
        print(&c->out, (i64)s.w);
        putbyte(&c->out, ' ');
        print(&c->out, (i64)s.h);
        print(&c->out, " 0x");
        if (w.id) {
            printhex(&c->out, w.id, w.id>>32 ? 16 : 8);
        } else {
            putbyte(&c->out, '0');  // the screen
        }
        putbyte(&c->out, '\n');
    }
}

static void dkcmdwait(DkCtx *c, DkArgs *a)
{
    enum { EXISTS, GONE, CHANGE, IDLE } mode = EXISTS;
    i64 timeout = 10000;
    while (!c->status && dkmore(a) && a->v[a->i].len && a->v[a->i][0]=='-') {
        Str o = dknext(c, a, "option");
        if (o == "-gone") {
            mode = GONE;
        } else if (o == "-change") {
            mode = CHANGE;
        } else if (o == "-idle") {
            mode = IDLE;
        } else if (o == "-t") {
            Str t = dknext(c, a, "seconds");
            timeout = c->status ? 0 : dkparsems(t);
            if (!c->status && (timeout<0 || timeout>DK_WAITMAX)) {
                dkfail(c, DK_FAIL, "invalid timeout", t);
            }
        } else {
            dkfail(c, DK_FAIL, "unknown option", o);
        }
    }
    Str spec = dknext(c, a, "window");
    dkdone(c, a);
    if (c->status) {
        return;
    }
    DkSel s = dkparsesel(spec);
    if (s.screen || s.active) {
        dkfail(c, DK_FAIL, "cannot wait on", spec);
        return;
    }
    i64 start = dk_ms(c->dk);

    if (mode == IDLE) {
        u32 pid = s.pid;
        if (!pid || s.title.data || s.cls.data || s.id) {
            pid = dkfindone(c, spec).pid;
            if (c->status) {
                return;
            }
        }
        i64 left = timeout - (dk_ms(c->dk) - start);
        i32 r = dk_idle(c->dk, pid, left>0 ? (i32)left : 0);
        if (r == 1) {
            dkfail(c, DK_TIMEOUT, "timed out waiting for idle", spec);
        } else if (r) {
            dkfailwhy(c, "cannot wait for idle (not a GUI process?)", spec);
        }
        return;
    }

    DkWin orig = {};
    if (mode == CHANGE) {
        orig = dkfindone(c, spec);
        if (c->status) {
            return;
        }
    }

    // Each poll lists windows into an arena of its own, reset every
    // time, apart from the output and the original window
    Arena *keep = c->a;
    Arena  poll = {};
    iz     cap  = (iz)4 << 20;
    poll.beg = (byte *)allocbytes(keep, cap);
    poll.end = poll.beg + cap;
    for (;;) {
        Arena scratch = poll;
        c->a = &scratch;
        b32 done = 0;
        if (mode == CHANGE) {
            DkWin now = dk_window(c->dk, &scratch, orig.id);
            if (!now.id) {
                c->a = keep;
                dkfail(c, DK_NOMATCH, "window closed while waiting", spec);
                return;
            } else if (!(now.title == orig.title)) {
                c->a = keep;
                dkprintwin(&c->out, &now);
                return;
            }
        } else {
            Slice<DkWin> m = dkfindall(c, &s);
            if (mode==EXISTS && m.len) {
                c->a = keep;
                for (iz i = 0; i < m.len; i++) {
                    dkprintwin(&c->out, &m[i]);
                }
                return;
            }
            done = mode==GONE && !m.len;
        }
        c->a = keep;
        if (done) {
            return;
        } else if (dk_ms(c->dk)-start >= timeout) {
            dkfail(c, DK_TIMEOUT, "timed out waiting for", spec);
            return;
        }
        dk_sleep(c->dk, 50);
    }
}

struct DkPoint {
    i32 x, y;
};

static DkPoint dkreadpoint(DkCtx *c, DkArgs *a)
{
    DkPoint p = {};
    p.x = dknextint(c, a, "x");
    p.y = dknextint(c, a, "y");
    return p;
}

// A point in window coordinates on the screen.
static DkPoint dktoscreen(DkWin *w, DkPoint p)
{
    p.x += w->rect.left;
    p.y += w->rect.top;
    return p;
}

// click, dclick, rclick, and hover, which only moves the pointer.
static void dkcmdclick(DkCtx *c, DkArgs *a, u8 kind)
{
    DkWin   w = dkfindone(c, dknext(c, a, "window"));
    DkPoint p = dkreadpoint(c, a);
    dkdone(c, a);
    dkactivate(c, &w);
    if (c->status) {
        return;
    }
    Dk *dk = c->dk;
    p = dktoscreen(&w, p);
    dkinput(c, dk_pointer(dk, p.x, p.y));
    if (kind == 'h') {
        dk_sleep(dk, 50);
        return;
    }
    dk_sleep(dk, 30);
    i32 button = kind=='r';
    for (i32 n = 0; !c->status && n < (kind=='d' ? 2 : 1); n++) {
        if (n) dk_sleep(dk, 50);
        dkinput(c, dk_button(dk, button, 1));
        dk_sleep(dk, 20);
        dkinput(c, dk_button(dk, button, 0));
    }
    dk_sleep(dk, 50);
}

// Slow enough for drag detection (DragDetect, OLE DoDragDrop): single
// pixels until past the threshold, then the rest in steps with pauses,
// a hover at the drop point, and the release.
static void dkcmddrag(DkCtx *c, DkArgs *a)
{
    DkWin   w  = dkfindone(c, dknext(c, a, "window"));
    DkPoint p0 = dkreadpoint(c, a);
    DkPoint p1 = dkreadpoint(c, a);
    dkdone(c, a);
    dkactivate(c, &w);
    if (c->status) {
        return;
    }
    Dk *dk = c->dk;
    p0 = dktoscreen(&w, p0);
    p1 = dktoscreen(&w, p1);

    i32 dx = p1.x - p0.x;
    i32 dy = p1.y - p0.y;
    i32 ax = dx<0 ? -dx : dx;
    i32 ay = dy<0 ? -dy : dy;
    i32 d  = ax>ay ? ax : ay;
    dkinput(c, dk_pointer(dk, p0.x, p0.y));
    dk_sleep(dk, 100);
    dkinput(c, dk_button(dk, 0, 1));
    dk_sleep(dk, 100);

    i32 thr = dk_dragstep(dk)*2 + 2;
    i32 k   = 1;
    for (; !c->status && k<=d && k<=thr; k++) {
        dkinput(c, dk_pointer(dk, p0.x + (i32)((i64)dx*k/d), p0.y + (i32)((i64)dy*k/d)));
        dk_sleep(dk, 20);
    }
    i32 steps = 20;
    for (i32 s = 1; !c->status && k<=d && s<=steps; s++) {
        i32 j = k + (i32)((i64)(d - k)*s/steps);
        dkinput(c, dk_pointer(dk, p0.x + (i32)((i64)dx*j/d), p0.y + (i32)((i64)dy*j/d)));
        dk_sleep(dk, 25);
    }
    dkinput(c, dk_pointer(dk, p1.x, p1.y));
    dk_sleep(dk, 150);  // let the drop target see the hover
    dkinput(c, dk_pointer(dk, p1.x, p1.y));
    dk_sleep(dk, 100);
    // Release even after a failure, so that no button stays held
    b32 up = dk_button(dk, 0, 0);
    dkinput(c, up);
    dk_sleep(dk, 100);
}

static void dkcmdwheel(DkCtx *c, DkArgs *a)
{
    DkWin   w = dkfindone(c, dknext(c, a, "window"));
    DkPoint p = dkreadpoint(c, a);
    i32     n = dknextint(c, a, "clicks");
    dkdone(c, a);
    if (!c->status && (n<-1000 || n>1000)) {
        dkfail(c, DK_FAIL, "too many clicks: at most 1000");
    }
    dkactivate(c, &w);
    if (c->status) {
        return;
    }
    p = dktoscreen(&w, p);
    dkinput(c, dk_pointer(c->dk, p.x, p.y));
    dk_sleep(c->dk, 30);
    dkinput(c, dk_wheel(c->dk, n));
    dk_sleep(c->dk, 50);
}

struct DkChord {
    i32 codes[8];  // modifiers, then the key
    i32 len;
    b32 ok;
};

// Keys joined by +. Letters never imply shift: ctrl+S is ctrl+s.
static DkChord dkparsechord(Dk *dk, Str s)
{
    DkChord r = {};
    for (Str rest = s; rest.len;) {
        Cut c = cut(rest, '+');
        Str k = c.head;
        rest = c.tail;
        if (!k.len && c.ok && !rest.len) {
            k = "+";  // a trailing +, as in ctrl++
        }
        if (r.len==countof(r.codes) || !k.len) {
            return r;
        }
        b32 shift = 0;
        i32 code  = dk_keycode(dk, k, &shift);
        if (code < 0) {
            return r;
        }
        if (shift) {
            i32 sc = dk_keycode(dk, "shift", &shift);
            if (sc<0 || r.len==countof(r.codes)-1) {
                return r;
            }
            r.codes[r.len++] = sc;
        }
        r.codes[r.len++] = code;
    }
    r.ok = r.len > 0;
    return r;
}

static void dkcmdkey(DkCtx *c, DkArgs *a)
{
    DkWin w = dkfindone(c, dknext(c, a, "window"));
    if (!c->status && !dkmore(a)) {
        dkfail(c, DK_FAIL, "missing argument:", "key");
    }
    Slice<DkChord> chords = {};
    while (!c->status && dkmore(a)) {
        Str     s = dknext(c, a, "key");
        DkChord k = dkparsechord(c->dk, s);
        if (!k.ok) {
            dkfail(c, DK_FAIL, "unknown key", s);
        }
        chords = push(c->a, chords, k);
    }
    dkactivate(c, &w);
    for (iz i = 0; !c->status && i < chords.len; i++) {
        if (i) dk_sleep(c->dk, 50);
        dkinput(c, dk_keys(c->dk, chords[i].codes, chords[i].len));
    }
    dk_sleep(c->dk, 50);
}

static Str dkreadstdin(Dk *dk, Arena *a)
{
    Str r = {};
    r.data = allocbytes(a, 0);
    for (;;) {
        iz cap = 1<<16;
        allocbytes(a, cap);  // extends r in place
        iz got = dk_read(dk, r.data+r.len, cap);
        if (got <= 0) {
            break;
        }
        r.len += got;
        a->beg = (byte *)(r.data + r.len);
    }
    a->beg = (byte *)(r.data + r.len);
    return r;
}

static void dkcmdtype(DkCtx *c, DkArgs *a)
{
    DkWin w    = dkfindone(c, dknext(c, a, "window"));
    Str   text = dknext(c, a, "text");
    dkdone(c, a);
    if (!c->status && !c->serving && text=="-") {
        text = dkreadstdin(c->dk, c->a);
    }
    dkactivate(c, &w);
    if (!c->status) {
        dkinput(c, dk_text(c->dk, *c->a, text));
        dk_sleep(c->dk, 50);
    }
}

static void dkcmdraise(DkCtx *c, DkArgs *a)
{
    DkWin w = dkfindone(c, dknext(c, a, "window"));
    dkdone(c, a);
    dkactivate(c, &w);
}

static void dkcmdclose(DkCtx *c, DkArgs *a)
{
    DkWin w = dkfindone(c, dknext(c, a, "window"));
    dkdone(c, a);
    if (!c->status && !w.id) {
        dkfail(c, DK_FAIL, "cannot close the screen");
    } else if (!c->status && !dk_close(c->dk, &w)) {
        dkfailwhy(c, "cannot close", w.title);
    }
}

static void dkcmdmove(DkCtx *c, DkArgs *a)
{
    DkWin w    = dkfindone(c, dknext(c, a, "window"));
    i32   x    = dknextint(c, a, "x");
    i32   y    = dknextint(c, a, "y");
    b32   size = dkmore(a);
    i32   cw   = size ? dknextint(c, a, "width")  : 0;
    i32   ch   = size ? dknextint(c, a, "height") : 0;
    dkdone(c, a);
    if (c->status) {
        return;
    } else if (!w.id) {
        dkfail(c, DK_FAIL, "cannot move the screen");
    } else if (size && (cw<=0 || ch<=0)) {
        dkfail(c, DK_FAIL, "width and height must be positive");
    } else if (!dk_move(c->dk, &w, x, y, cw, ch, size)) {
        dkfailwhy(c, "cannot move window", w.title);
    }
}

static void dkcmdrun(DkCtx *c, DkArgs *a)
{
    if (!dkmore(a)) {
        dkfail(c, DK_FAIL, "missing argument:", "program");
        return;
    }
    Slice<Str> argv = {};
    for (; dkmore(a); a->i++) {
        argv = push(c->a, argv, a->v[a->i]);
    }
    b32 attached = 0;
    u32 pid      = dk_run(c->dk, *c->a, argv, &attached);
    if (!pid) {
        dkfailwhy(c, "cannot start", argv[0]);
        return;
    }
    print(&c->out, (i64)pid);
    putbyte(&c->out, '\n');
    if (attached) {
        // Windows: it could not leave the job; macOS: no application;
        // Linux: no user service manager. So it stays in the reaper's
        // tree, which ends with the harness: in serve mode, the line
        // after the pid says so
        Str why = dk_why(c->dk);
        Buf b(c->a, 128 + why.len);
        print(&b, "cannot detach the program, which will end with the session");
        if (why.data) {
            print(&b, ": ");
            print(&b, why);
        }
        putbyte(&b, '\n');
        if (c->serving) {
            print(&c->out, finish(&b));
        } else {
            dk_write(c->dk, 2, DK_NAME ": warning: ");
            dk_write(c->dk, 2, finish(&b));
        }
    }
}

static void dkcmdaccess(DkCtx *c, DkArgs *a)
{
    b32 ask = 0;
    if (dkmore(a) && a->v[a->i]=="-ask") {
        ask = 1;
        a->i++;
    }
    dkdone(c, a);
    if (!c->status) {
        dk_access(c->dk, &c->out, ask);
    }
}

static Str dkusage =
"usage: " DK_NAME " COMMAND [ARGS]\n"
"       " DK_NAME " --serve | --version\n"
"  list [-a] [WINDOW]           list visible top-level windows (-a: all)\n"
"  shot [-s EDGE PIXELS] [WINDOW [X Y W H]]\n"
"                               PNG of the screen under WINDOW to stdout,\n"
"                               scaled to fit EDGE and PIXELS (0: any)\n"
"  wait [-gone|-change|-idle] [-t SECS] WINDOW\n"
"                               wait until WINDOW exists, is gone, changes\n"
"                               title, or its process is idle (default 10s)\n"
"  click|dclick|rclick|hover WINDOW X Y\n"
"  drag WINDOW X0 Y0 X1 Y1\n"
"  wheel WINDOW X Y CLICKS      positive scrolls up\n"
"  key WINDOW CHORD...          e.g. ctrl+s alt+f shift+tab enter f2 del\n"
"  type WINDOW TEXT             TEXT as Unicode keys, - reads stdin (UTF-8)\n"
"  raise WINDOW                 bring to the foreground\n"
"  close WINDOW                 ask the window to close\n"
"  move WINDOW X Y [W H]        move and resize, in screen coordinates\n"
"  run PROGRAM [ARGS...]        start detached from the session, print PID\n"
"  access [-ask]                the permissions needed, and whether granted\n"
"                               (-ask: ask the user for those missing)\n"
"  --serve                      take commands as frames on standard input\n"
"WINDOW: title substring, or key=value[,key=value...] with keys title=,\n"
"  class=, pid=, hwnd=; or 'screen'; or 'active' (the foreground window).\n"
"Coordinates are relative to the window, as in its shot: pixels on Windows\n"
"  and X11, points on macOS.\n"
"Exit status: 0 ok, 1 error, 2 no or ambiguous window match, 3 timeout.\n";

// Run one command, its output in c.
static void dkcommand(DkCtx *c, DkArgs *a)
{
    dk_why(c->dk);  // a reason left from an earlier command is stale
    Str cmd = dknext(c, a, "command");
    if (c->status) {
        return;
    } else if (cmd=="-h" || cmd=="--help" || cmd=="help") {
        print(&c->out, dkusage);
        return;
    } else if (cmd=="hello" && c->serving) {
        print(&c->out, "prospero-desktop ");
        print(&c->out, (i64)DK_PROTO);
        putbyte(&c->out, ' ');
        print(&c->out, dk_os(c->dk));
        putbyte(&c->out, ' ');
        print(&c->out, dk_units(c->dk));
        putbyte(&c->out, '\n');
        return;
    }

    // Every other command needs the desktop, the hello only the helper
    Str why = dk_ready(c->dk);
    if (why.data) {
        dkfail(c, DK_FAIL, why);
    } else if (cmd == "list") {
        dkcmdlist(c, a);
    } else if (cmd == "shot") {
        dkcmdshot(c, a);
    } else if (cmd == "wait") {
        dkcmdwait(c, a);
    } else if (cmd == "click") {
        dkcmdclick(c, a, 'l');
    } else if (cmd == "dclick") {
        dkcmdclick(c, a, 'd');
    } else if (cmd == "rclick") {
        dkcmdclick(c, a, 'r');
    } else if (cmd == "hover") {
        dkcmdclick(c, a, 'h');
    } else if (cmd == "drag") {
        dkcmddrag(c, a);
    } else if (cmd == "wheel") {
        dkcmdwheel(c, a);
    } else if (cmd == "key") {
        dkcmdkey(c, a);
    } else if (cmd == "type") {
        dkcmdtype(c, a);
    } else if (cmd == "raise") {
        dkcmdraise(c, a);
    } else if (cmd == "close") {
        dkcmdclose(c, a);
    } else if (cmd == "move") {
        dkcmdmove(c, a);
    } else if (cmd == "run") {
        dkcmdrun(c, a);
    } else if (cmd == "access") {
        dkcmdaccess(c, a);
    } else {
        dkfail(c, DK_FAIL, "unknown command", cmd);
    }
}


// Frames

static void dkput32le(u8 *p, u32 v)
{
    p[0] = (u8)(v >>  0);
    p[1] = (u8)(v >>  8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}

static u32 dkget32le(u8 *p)
{
    return (u32)p[0] | (u32)p[1]<<8 | (u32)p[2]<<16 | (u32)p[3]<<24;
}

// The arguments of a request frame's payload: each ends in a null byte.
static Slice<Str> dksplitargs(Arena *a, Str payload)
{
    Slice<Str> r = {};
    for (Str rest = payload; rest.len;) {
        Cut c = cut(rest, 0);
        if (!c.ok) {
            break;  // unterminated: not an argument
        }
        r    = push(a, r, c.head);
        rest = c.tail;
    }
    return r;
}

// A command's reply as a frame.
static Str dkreply(Arena *a, i32 status, Str text, Str data)
{
    iz  len = 1 + 4 + text.len + data.len;
    u8 *p   = allocbytes(a, 4 + len);
    dkput32le(p, (u32)len);
    p[4] = (u8)status;
    dkput32le(p+5, (u32)text.len);
    copybytes(p+9, text.data, text.len);
    copybytes(p+9+text.len, data.data, data.len);
    return {p, 4 + len};
}

// Read exactly len bytes of standard input. 0 at its end or on error.
static b32 dkreadall(Dk *dk, u8 *buf, iz len)
{
    for (iz off = 0; off < len;) {
        iz n = dk_read(dk, buf+off, len-off);
        if (n <= 0) {
            return 0;
        }
        off += n;
    }
    return 1;
}

// Run the command of one request payload, returning its reply frame.
static Str dkserveone(Dk *dk, Arena *a, Str payload)
{
    DkCtx c = {};
    c.dk      = dk;
    c.a       = a;
    c.out     = Buf(a, 1<<10);
    c.err     = Buf(a, 256);
    c.serving = 1;
    DkArgs args = {};
    Slice<Str> v = dksplitargs(a, payload);
    args.v = v.data;
    args.n = (i32)v.len;
    dkcommand(&c, &args);
    if (c.status) {
        return dkreply(a, c.status, finish(&c.err), {});
    }
    return dkreply(a, DK_OK, finish(&c.out), c.data);
}

// Take request frames from standard input until its end, answering
// each in turn.
static i32 dkserve(Dk *dk, Arena scratch)
{
    for (;;) {
        Arena a = scratch;
        u8    head[4];
        if (!dkreadall(dk, head, 4)) {
            return 0;  // the harness is done
        }
        u32 len = dkget32le(head);
        if (len > DK_REQMAX) {
            dk_write(dk, 2, DK_NAME ": request too long\n");
            return 1;
        }
        u8 *buf = allocbytes(&a, len);
        if (!dkreadall(dk, buf, len)) {
            return 1;
        }
        Str reply = dkserveone(dk, &a, {buf, (iz)len});
        if (!dk_write(dk, 1, reply)) {
            return 1;
        }
    }
}

// The entry point: one command from args, or --serve.
static i32 dkmain(Dk *dk, Arena scratch, Str *args, i32 nargs)
{
    if (nargs==2 && args[1]=="--serve") {
        return dkserve(dk, scratch);
    } else if (nargs==2 && args[1]=="--version") {
        // Touches no desktop: deploy runs it to see that it runs there
        return dk_write(dk, 1, "prospero-desktop " PROSPERO_VERSION "\n") ? DK_OK : DK_FAIL;
    } else if (nargs < 2) {
        dk_write(dk, 2, dkusage);
        return DK_FAIL;
    }
    DkCtx c = {};
    c.dk  = dk;
    c.a   = &scratch;
    c.out = Buf(&scratch, 1<<12);
    c.err = Buf(&scratch, 256);
    DkArgs a = {};
    a.v = args;
    a.n = nargs;
    a.i = 1;
    dkcommand(&c, &a);
    if (c.status) {
        Str msg = finish(&c.err);
        Buf b(&scratch, 32 + msg.len);
        print(&b, DK_NAME ": ");
        print(&b, msg);
        putbyte(&b, '\n');
        dk_write(dk, 2, finish(&b));
        return c.status;
    }
    b32 ok = dk_write(dk, 1, finish(&c.out));
    ok &= !c.data.len || dk_write(dk, 1, c.data);
    return ok ? DK_OK : DK_FAIL;
}
