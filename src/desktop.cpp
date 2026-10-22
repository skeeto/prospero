// Desktop control: the desktop tool's side of the helper (deskhelp.cpp)
//
// The desktop tool lets an agent see the screen and use the mouse and
// keyboard. The harness does none of it itself: a helper program,
// prospero-desktop, next to the executable, takes commands over pipes
// (the frames deskhelp.cpp describes), started when a session first
// needs it and stopped when its turn ends. Only Windows has a helper so
// far; elsewhere the tool says that it is not available.
//
// Points are in the latest screenshot, as the model sees its image: a
// screenshot is scaled down to the model's limits ([model] image_edge
// and image_pixels), and a model gives points in pixels of that image,
// or in thousandths of its width and height (norm1000, Qwen-VL's and
// Gemini's way), per [model] coordinates. The harness maps them back to
// the pixels of the window or screen captured, which the helper takes,
// and the helper adds where that window is now. Input with no window of
// its own goes where the screenshot was taken.
//
// Old screenshots leave requests in steps ([desktop] images and
// images_slack, deskdropped), each replaced by a short line naming its
// call, so that the prompt cache hits between steps; for Anthropic, the
// request at a step marks where its prefix still matches the one before
// (anthropic.cpp).
//
// The tool is off unless [desktop] offer turns it on, which only the
// user's own file can do, and every action is an ask with a deadline,
// denied when nobody answers, unless [desktop] ask is false.
//
// Everything here is pure: the engine does the I/O.
//
// This is free and unencumbered software released into the public domain.

enum : i32 {
    COORDS_PIXELS,    // pixels of the image the model was sent
    COORDS_NORM1000,  // thousandths of its width and height
};

enum : i32 {
    DESK_SCREENSHOT,
    DESK_CLICK,
    DESK_DOUBLE,
    DESK_RIGHT,
    DESK_MOVE,
    DESK_DRAG,
    DESK_SCROLL,
    DESK_KEY,
    DESK_TYPE,
    DESK_WINDOWS,
    DESK_FOCUS,
    DESK_CLOSE,
    DESK_WAIT,
    DESK_RUN,
    DESK_COUNT,
};

static Str desknames[] = {
    "screenshot", "click", "double_click", "right_click", "mouse_move",
    "drag", "scroll", "key", "type", "windows", "focus", "close", "wait",
    "run",
};
static_assert(countof(desknames) == DESK_COUNT);

enum : i64 {
    DESK_PROTO     = 1,
    DESK_WAITMAX   = 30,                // seconds a wait may take
    DESK_TEXTMAX   = 64 << 10,          // bytes typed at once
    DESK_REPLYMAX  = (i64)64 << 20,     // largest reply frame
    DESK_EDGE      = 1568,              // image limits for kind anthropic
    DESK_PIXELS    = 1150000,
    DESK_EDGE_OAI  = 1280,              // and for the others
    DESK_PIXELS_OAI = 1048576,
    DESK_IMAGES    = 3,                 // screenshots kept, by default
    DESK_SLACK     = 4,                 // and more before any are dropped
};

// The arguments of a desktop call.
struct DeskArgs {
    i32        action;  // DESK_*, -1 if none
    f64        x, y, x2, y2;
    b32        point;   // x and y given
    b32        point2;  // x2 and y2 given
    i64        amount;
    Str        keys;
    Str        text;
    Str        window;
    Str        program;
    Slice<Str> args;
    b32        gone;
    i64        secs;    // wait, -1 if absent
};

static i32 deskaction(Str name)
{
    name = trim(name);
    for (i32 i = 0; i < DESK_COUNT; i++) {
        if (equalfold(name, desknames[i])) {
            return i;
        }
    }
    static struct {
        Str name;
        i32 action;
    } const aliases[] = {
        {"screen", DESK_SCREENSHOT}, {"capture", DESK_SCREENSHOT},
        {"left_click", DESK_CLICK}, {"doubleclick", DESK_DOUBLE},
        {"double", DESK_DOUBLE}, {"rightclick", DESK_RIGHT},
        {"move", DESK_MOVE}, {"hover", DESK_MOVE}, {"mouse", DESK_MOVE},
        {"left_click_drag", DESK_DRAG}, {"wheel", DESK_SCROLL},
        {"keys", DESK_KEY}, {"key_press", DESK_KEY}, {"press", DESK_KEY},
        {"write", DESK_TYPE}, {"type_text", DESK_TYPE},
        {"list", DESK_WINDOWS}, {"list_windows", DESK_WINDOWS},
        {"activate", DESK_FOCUS}, {"raise", DESK_FOCUS},
        {"wait_for", DESK_WAIT}, {"launch", DESK_RUN}, {"open", DESK_RUN},
    };
    for (iz i = 0; i < countof(aliases); i++) {
        if (equalfold(name, aliases[i].name)) {
            return aliases[i].action;
        }
    }
    return -1;
}

// Actions that take a point, and those with a second one.
static b32 deskneedspoint(i32 action)
{
    return action==DESK_CLICK || action==DESK_DOUBLE || action==DESK_RIGHT ||
           action==DESK_MOVE  || action==DESK_DRAG   || action==DESK_SCROLL;
}


// Coordinates

// The latest screenshot's geometry, which points refer to.
struct DeskShot {
    Str target;  // the helper's window: "screen", or "hwnd=0x..."
    i32 ox, oy;  // where the capture starts in the target's coordinates
    i32 w, h;    // pixels captured
    i32 iw, ih;  // the image's size
    b32 ok;
};

// A whole hexadecimal number with its 0x, or -1.
static i64 deskhex(Str s)
{
    if (s.len<3 || s.len>18 || s[0]!='0' || lowercase(s[1])!='x') {
        return -1;
    }
    u64 v = 0;
    for (iz i = 2; i < s.len; i++) {
        u8  c = lowercase(s[i]);
        i32 d = digit(c) ? c-'0' : c>='a' && c<='f' ? c-'a'+10 : -1;
        if (d < 0) {
            return -1;
        }
        v = v<<4 | (u64)d;
    }
    return v>(u64)I64MAX ? -1 : (i64)v;
}

// A shot's geometry from the helper's text, "OX OY W H IW IH ID".
static DeskShot deskshotparse(Arena *a, Str text)
{
    DeskShot r = {};
    i64      v[6] = {};
    Str      rest = trim(text);
    for (i32 i = 0; i < 6; i++) {
        Cut c = cut(rest, ' ');
        Int n = parseint(trim(c.head));
        if (!n.ok || n.val<-(1<<24) || n.val>(1<<24)) {
            return r;
        }
        v[i] = n.val;
        rest = trim(c.tail);
    }
    i64 id = deskhex(rest);
    r.ox = (i32)v[0];
    r.oy = (i32)v[1];
    r.w  = (i32)v[2];
    r.h  = (i32)v[3];
    r.iw = (i32)v[4];
    r.ih = (i32)v[5];
    r.ok = id>=0 && r.w>0 && r.h>0 && r.iw>0 && r.ih>0 &&
           r.iw<=r.w && r.ih<=r.h;
    if (r.ok && id) {
        Buf b(a, 32);
        print(&b, "hwnd=0x");
        printhex(&b, (u64)id, id>>32 ? 16 : 8);
        r.target = finish(&b);
    } else {
        r.target = "screen";
    }
    return r;
}

// The geometry as deskshotparse takes it.
static Str deskshotstr(Arena *a, DeskShot *s)
{
    Buf b(a, 64);
    i64 v[] = {s->ox, s->oy, s->w, s->h, s->iw, s->ih};
    for (iz i = 0; i < countof(v); i++) {
        if (i) putbyte(&b, ' ');
        print(&b, v[i]);
    }
    print(&b, " 0x");
    Str t = s->target;
    print(&b, startswith(t, "hwnd=0x") ? cuthead(t, 7) : Str("0"));
    return finish(&b);
}

struct DeskPoint {
    i32 x, y;  // in the target's coordinates
    Str why;   // null if fine, else what is wrong with the point
};

static i64 deskround(f64 v)
{
    return v<0 ? -(i64)(0.5 - v) : (i64)(v + 0.5);
}

// One coordinate in the image (0 to 1000 for norm1000, else a pixel
// from 0 to n-1 of an image n wide) to the captured pixels (0 to m-1,
// m at least n), or -1 if out of range.
static i32 deskmap(i32 coords, f64 v, i32 n, i32 m)
{
    i64 r = 0;
    if (coords == COORDS_NORM1000) {
        if (v<0 || v>1000) return -1;
        r = deskround(v * m / 1000);
    } else {
        if (v<0 || v>=n) return -1;
        // The middle of the captured pixels that scale to this one, as
        // deskmodel scales them: [ceil(x*m/n), ceil((x+1)*m/n))
        i64 x  = (i64)v;
        i64 lo = (x*m + n - 1) / n;
        i64 hi = ((x+1)*m + n - 1) / n - 1;
        r = (lo + hi) / 2;
    }
    return (i32)(r<m ? r : m-1);
}

// A point the model gave in the latest screenshot, in the target's
// coordinates, as the helper takes them.
static DeskPoint desktarget(Arena *a, DeskShot *s, i32 coords, f64 x, f64 y)
{
    DeskPoint r  = {};
    i32       cx = deskmap(coords, x, s->iw, s->w);
    i32       cy = deskmap(coords, y, s->ih, s->h);
    if (cx<0 || cy<0) {
        Buf b(a, 128);
        print(&b, "the point (");
        printfloat(&b, x, 0);
        print(&b, ", ");
        printfloat(&b, y, 0);
        print(&b, ") is outside the screenshot: ");
        if (coords == COORDS_NORM1000) {
            print(&b, "x and y go from 0 to 1000");
        } else {
            print(&b, "x goes from 0 to ");
            print(&b, (i64)(s->iw - 1));
            print(&b, " and y from 0 to ");
            print(&b, (i64)(s->ih - 1));
        }
        r.why = finish(&b);
        return r;
    }
    r.x = s->ox + cx;
    r.y = s->oy + cy;
    return r;
}

// A point in the target's coordinates as the model would give it: the
// inverse of desktarget, to the nearest unit.
struct DeskXY {
    i64 x, y;
};

static DeskXY deskmodel(DeskShot *s, i32 coords, i32 tx, i32 ty)
{
    DeskXY r  = {};
    i64    cx = tx - s->ox;
    i64    cy = ty - s->oy;
    if (coords == COORDS_NORM1000) {
        r.x = deskround((f64)cx * 1000 / s->w);
        r.y = deskround((f64)cy * 1000 / s->h);
    } else {
        r.x = cx * s->iw / s->w;
        r.y = cy * s->ih / s->h;
    }
    return r;
}

// A window as the agent names it, as the helper takes it: "id=" is the
// helper's "hwnd=", and anything else goes as written.
static Str deskwindow(Arena *a, Str w)
{
    w = trim(w);
    Buf b(a, w.len + 8);
    for (Str rest = w; rest.len;) {
        Cut c = cut(rest, ',');
        Str p = trim(c.head);
        rest  = c.tail;
        if (b.len) putbyte(&b, ',');
        if (startswith(p, "id=")) {
            print(&b, "hwnd=");
            p = cuthead(p, 3);
        }
        print(&b, p);
    }
    return finish(&b);
}


// The helper's frames

static void deskput32(u8 *p, u32 v)
{
    p[0] = (u8)(v >>  0);
    p[1] = (u8)(v >>  8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}

static u32 deskget32(u8 *p)
{
    return (u32)p[0] | (u32)p[1]<<8 | (u32)p[2]<<16 | (u32)p[3]<<24;
}

// A request frame: the arguments, each ending in a null byte.
static Str deskframe(Arena *a, Slice<Str> argv)
{
    iz len = 0;
    for (iz i = 0; i < argv.len; i++) {
        len += argv[i].len + 1;
    }
    u8 *p = allocbytes(a, 4 + len);
    deskput32(p, (u32)len);
    iz n = 4;
    for (iz i = 0; i < argv.len; i++) {
        copybytes(p+n, argv[i].data, argv[i].len);
        n += argv[i].len;
        p[n++] = 0;
    }
    return {p, n};
}

// The size of the whole frame at the start of buf: 0 if it has not all
// arrived, -1 if it is larger than max.
static iz deskframelen(Str buf, iz max)
{
    if (buf.len < 4) {
        return 0;
    }
    i64 len = deskget32(buf.data);
    if (len > max) {
        return -1;
    }
    return buf.len>=4+len ? (iz)(4 + len) : 0;
}

struct DeskReply {
    i32 status;  // the helper's: 0 ok, 1 error, 2 no window, 3 timeout
    Str text;
    Str data;
    b32 ok;      // the frame was well formed
};

// A whole reply frame, length included.
static DeskReply deskreply(Str frame)
{
    DeskReply r = {};
    if (frame.len < 4+5) {
        return r;
    }
    Str p   = cuthead(frame, 4);
    i64 len = deskget32(p.data+1);
    if (len > p.len-5) {
        return r;
    }
    r.status = p[0];
    r.text   = takehead(cuthead(p, 5), (iz)len);
    r.data   = cuthead(p, 5+(iz)len);
    r.ok     = 1;
    return r;
}

// Base64, for images in requests.
static Str base64(Arena *a, Str s)
{
    static u8 const digits[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    iz  len = (s.len + 2) / 3 * 4;
    u8 *p   = allocbytes(a, len);
    iz  n   = 0;
    for (iz i = 0; i < s.len; i += 3) {
        u32 v = (u32)s[i] << 16;
        v |= i+1<s.len ? (u32)s[i+1] << 8 : 0;
        v |= i+2<s.len ? (u32)s[i+2]      : 0;
        p[n++] = digits[v>>18 & 63];
        p[n++] = digits[v>>12 & 63];
        p[n++] = i+1<s.len ? digits[v>>6 & 63] : '=';
        p[n++] = i+2<s.len ? digits[v>>0 & 63] : '=';
    }
    return {p, n};
}

// The size of a PNG from its header, or 0 by 0 if it is not one.
struct DeskSize {
    i32 w, h;
};

static DeskSize pngsize(Str png)
{
    DeskSize r = {};
    static u8 const sig[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    if (png.len<24 || !startswith(png, Str{(u8 *)sig, 8}) ||
            !(takehead(cuthead(png, 12), 4) == "IHDR")) {
        return r;
    }
    u8 *p = png.data + 16;
    u32 w = (u32)p[0]<<24 | (u32)p[1]<<16 | (u32)p[2]<<8 | p[3];
    u32 h = (u32)p[4]<<24 | (u32)p[5]<<16 | (u32)p[6]<<8 | p[7];
    if (w && h && w<(1<<16) && h<(1<<16)) {
        r.w = (i32)w;
        r.h = (i32)h;
    }
    return r;
}

// Tokens an image costs a model, about: Anthropic's width*height/750.
static i64 imagetokens(i32 w, i32 h)
{
    return (i64)w*h/750 + 1;
}

// Screenshots dropped from requests, the oldest first, of total in the
// conversation: none until there are keep+slack, then back to keep all
// at once, so that a request's prefix changes once every slack
// screenshots rather than at each one, and the prompt cache keeps
// hitting in between. A function of the count alone, so that the same
// history always gives the same requests.
static i64 deskdropped(i64 total, i64 keep, i64 slack)
{
    if (keep <= 0) {
        return total;
    } else if (total <= keep) {
        return 0;
    } else if (slack <= 0) {
        return total - keep;
    }
    return (total - keep)/slack * slack;
}

// Actions that only look: the loop guard counts them as repeated only
// while what they see stays the same (deskseen), since looking again at
// a screen that changes is how an agent waits for it.
static b32 desklooks(i32 action)
{
    return action==DESK_SCREENSHOT || action==DESK_WINDOWS || action==DESK_WAIT;
}

// What a step's looks saw so far, h, and then this reply.
static u64 deskseen(u64 h, DeskReply *r)
{
    u64 x = hash(r->text) ^ hash(r->data)*0x9e3779b97f4a7c15u ^ (u64)r->status;
    return (h ^ x) * 1111111111111111111u;
}
