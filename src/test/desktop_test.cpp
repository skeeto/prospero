// Desktop tests: points mapped both ways, the helper's frames, the
// tool's arguments and schema, screenshots in requests, and the
// helper's own core (deskhelp.cpp) over a fake desktop


// A fake desktop for the helper's core: a screen, three windows, and a
// log of the input it is sent.
struct Dk {
    Arena *a;
    DkRect screen;
    DkWin  wins[3];
    u64    fg;      // the foreground window
    Buf    log;     // input, one event a line
    i64    now;     // milliseconds
    Str    stdin_;  // what dk_read reads
    Buf    out[3];  // what dk_write wrote
};

static Str dk_os(Dk *) { return "fake"; }

static Slice<DkWin> dk_windows(Dk *dk, Arena *a, b32)
{
    Slice<DkWin> r = {};
    for (i32 i = 0; i < countof(dk->wins); i++) {
        r = push(a, r, dk->wins[i]);
    }
    return r;
}

static DkWin dk_window(Dk *dk, Arena *, u64 id)
{
    for (i32 i = 0; i < countof(dk->wins); i++) {
        if (dk->wins[i].id == id) {
            return dk->wins[i];
        }
    }
    return {};
}

static DkWin dk_screen(Dk *dk)
{
    DkWin r = {};
    r.rect  = dk->screen;
    r.title = "screen";
    return r;
}

static u64 dk_foreground(Dk *dk) { return dk->fg; }
static b32 dk_popup(Dk *, DkWin *w) { return w->cls == "#32768"; }

static b32 dk_activate(Dk *dk, DkWin *w)
{
    dk->fg = w->id;
    print(&dk->log, "raise 0x");
    printhex(&dk->log, w->id, 4);
    putbyte(&dk->log, '\n');
    return 1;
}

// A gradient: blue rises to the right, green downward.
static u8 *dk_capture(Dk *, Arena *a, DkRect r)
{
    i32 w = r.right - r.left;
    i32 h = r.bottom - r.top;
    u8 *p = allocbytes(a, (iz)w*h*4);
    for (i32 y = 0; y < h; y++) {
        for (i32 x = 0; x < w; x++) {
            u8 *q = p + ((iz)y*w + x)*4;
            q[0] = (u8)(r.left + x);
            q[1] = (u8)(r.top + y);
            q[2] = 0x80;
            q[3] = 0;
        }
    }
    return p;
}

static void dklogxy(Dk *dk, Str what, i32 x, i32 y)
{
    print(&dk->log, what);
    putbyte(&dk->log, ' ');
    print(&dk->log, (i64)x);
    putbyte(&dk->log, ',');
    print(&dk->log, (i64)y);
    putbyte(&dk->log, '\n');
}

static b32 dk_pointer(Dk *dk, i32 x, i32 y)
{
    dklogxy(dk, "move", x, y);
    return 1;
}

static b32 dk_button(Dk *dk, i32 button, b32 down)
{
    print(&dk->log, button ? Str("right ") : Str("left "));
    print(&dk->log, down ? Str("down\n") : Str("up\n"));
    return 1;
}

static b32 dk_wheel(Dk *dk, i32 n)
{
    dklogxy(dk, "wheel", n, 0);
    return 1;
}

static i32 dk_dragstep(Dk *) { return 2; }

static i32 dk_keycode(Dk *, Str name, b32 *shift)
{
    static Str const names[] = {"ctrl", "alt", "shift", "enter", "esc"};
    *shift = 0;
    for (i32 i = 0; i < countof(names); i++) {
        if (equalfold(name, names[i])) return 0x100 + i;
    }
    if (name.len == 1) {
        u8 c = name[0];
        *shift = c=='!' || (c>='A' && c<='Z');
        return c=='!' ? '1' : lowercase(c);
    }
    return -1;
}

static b32 dk_keys(Dk *dk, i32 *codes, i32 n)
{
    print(&dk->log, "keys");
    for (i32 i = 0; i < n; i++) {
        putbyte(&dk->log, ' ');
        print(&dk->log, (i64)codes[i]);
    }
    putbyte(&dk->log, '\n');
    return 1;
}

static b32 dk_text(Dk *dk, Arena, Str s)
{
    print(&dk->log, "text ");
    print(&dk->log, s);
    putbyte(&dk->log, '\n');
    return 1;
}

static b32 dk_close(Dk *dk, DkWin *w)
{
    print(&dk->log, "close ");
    print(&dk->log, w->title);
    putbyte(&dk->log, '\n');
    return 1;
}

static b32 dk_move(Dk *, DkWin *, i32, i32, i32, i32, b32) { return 1; }

static u32 dk_run(Dk *dk, Arena, Slice<Str> argv, b32 *attached)
{
    print(&dk->log, "run");
    for (iz i = 0; i < argv.len; i++) {
        putbyte(&dk->log, ' ');
        print(&dk->log, argv[i]);
    }
    putbyte(&dk->log, '\n');
    *attached = 0;
    return 4242;
}

static i32  dk_idle(Dk *, u32, i32) { return 0; }
static void dk_sleep(Dk *dk, i32 ms) { dk->now += ms; }
static i64  dk_ms(Dk *dk) { return dk->now; }

static iz dk_read(Dk *dk, u8 *buf, iz len)
{
    iz n = len<dk->stdin_.len ? len : dk->stdin_.len;
    copybytes(buf, dk->stdin_.data, n);
    dk->stdin_ = cuthead(dk->stdin_, n);
    return n;
}

static b32 dk_write(Dk *dk, i32 fd, Str s)
{
    print(&dk->out[fd], s);
    return 1;
}

static b32 dk_console(Dk *, i32) { return 0; }

static Dk *newdk(Arena *a)
{
    Dk *dk = alloc<Dk>(a);
    dk->a      = a;
    dk->screen = {0, 0, 1280, 800};
    dk->log    = Buf(a, 1024);
    for (i32 fd = 0; fd < 3; fd++) {
        dk->out[fd] = Buf(a, 1024);
    }
    DkWin w = {};
    w.id    = 0x10;
    w.pid   = 7;
    w.rect  = {100, 50, 740, 530};
    w.title = "Untitled - Notepad";
    w.cls   = "Notepad";
    dk->wins[0] = w;
    w.id    = 0x20;
    w.pid   = 7;
    w.rect  = {300, 200, 500, 300};
    w.title = "";
    w.cls   = "#32768";  // a popup menu of the same program
    dk->wins[1] = w;
    w.id    = 0x30;
    w.pid   = 9;
    w.rect  = {0, 0, 1280, 800};
    w.title = "Program Manager";
    w.cls   = "Progman";
    dk->wins[2] = w;
    return dk;
}

// A request's arguments, null-separated, as a frame for the helper.
static Str dkrequest(Arena *a, Str args)
{
    Slice<Str> v = {};
    for (Str rest = args; rest.len;) {
        Cut c = cut(rest, '|');
        v    = push(a, v, c.head);
        rest = c.tail;
    }
    return deskframe(a, v);
}

// Run one request through the helper's serve loop: "status|text".
static Str dkround(Arena *a, Dk *dk, Str args, Str *data = 0)
{
    Str       frame = dkrequest(a, args);
    Str       reply = dkserveone(dk, a, cuthead(frame, 4));
    DeskReply r     = deskreply(reply);
    if (data) {
        *data = r.data;
    }
    Buf b(a, 64 + r.text.len);
    print(&b, (i64)r.status);
    putbyte(&b, '|');
    print(&b, r.text);
    return finish(&b);
}

static Str dklog(Dk *dk)
{
    Str r = finish(&dk->log);
    dk->log.len = 0;
    return r;
}

static void test_desktop(Test *t)
{
    Arena *a = &t->perm;

    // Points: pixels of a scaled image go to the middle of their block
    // of captured pixels, and come back as the same pixel; thousandths
    // round to the nearest pixel
    {
        DeskShot s = {};
        s.target = "screen";
        s.w  = 2560;
        s.h  = 1440;
        s.iw = 1429;
        s.ih = 803;
        s.ok = 1;
        DeskPoint p = desktarget(a, &s, COORDS_PIXELS, 0, 0);
        expect(t, "px origin", concat(a, istr(a, p.x), istr(a, p.y)), "00");
        p = desktarget(a, &s, COORDS_PIXELS, 1428, 802);
        expect(t, "px corner", concat(a, concat(a, istr(a, p.x), ","), istr(a, p.y)),
               "2559,1439");
        p = desktarget(a, &s, COORDS_PIXELS, 714, 401);
        expect(t, "px middle", concat(a, concat(a, istr(a, p.x), ","), istr(a, p.y)),
               "1280,720");
        b32 back = 1;
        for (i32 x = 0; x < s.iw; x++) {
            DeskPoint q = desktarget(a, &s, COORDS_PIXELS, x, x%s.ih);
            DeskXY    m = deskmodel(&s, COORDS_PIXELS, q.x, q.y);
            back &= m.x==x && m.y==x%s.ih;
        }
        expect(t, "px round trip", istr(a, back), "1");
        p = desktarget(a, &s, COORDS_PIXELS, 1429, 0);
        expect(t, "px outside", p.why, "the point (1429, 0) is outside the "
               "screenshot: x goes from 0 to 1428 and y from 0 to 802");
        p = desktarget(a, &s, COORDS_PIXELS, -1, 5);
        expect(t, "px negative", istr(a, p.why.len > 0), "1");

        p = desktarget(a, &s, COORDS_NORM1000, 500, 500);
        expect(t, "norm middle", concat(a, concat(a, istr(a, p.x), ","), istr(a, p.y)),
               "1280,720");
        p = desktarget(a, &s, COORDS_NORM1000, 1000, 1000);
        expect(t, "norm corner", concat(a, concat(a, istr(a, p.x), ","), istr(a, p.y)),
               "2559,1439");
        back = 1;
        for (i32 n = 0; n < 1000; n++) {
            DeskPoint q = desktarget(a, &s, COORDS_NORM1000, n, 999-n);
            DeskXY    m = deskmodel(&s, COORDS_NORM1000, q.x, q.y);
            back &= m.x==n && m.y==999-n;
        }
        expect(t, "norm round trip", istr(a, back), "1");
        p = desktarget(a, &s, COORDS_NORM1000, 1000.5, 3);
        expect(t, "norm outside", p.why, "the point (1001, 3) is outside the "
               "screenshot: x and y go from 0 to 1000");

        // A window's capture, offset in it: the offset is added, and
        // the reverse takes it away
        DeskShot w = {};
        w.target = "hwnd=0x00000010";
        w.ox = 8;
        w.oy = 30;
        w.w  = 640;
        w.h  = 480;
        w.iw = 640;
        w.ih = 480;
        w.ok = 1;
        p = desktarget(a, &w, COORDS_PIXELS, 10, 20);
        expect(t, "window offset", concat(a, concat(a, istr(a, p.x), ","), istr(a, p.y)),
               "18,50");
        DeskXY m = deskmodel(&w, COORDS_PIXELS, 18, 50);
        expect(t, "window back", concat(a, concat(a, istr(a, m.x), ","), istr(a, m.y)),
               "10,20");
        m = deskmodel(&w, COORDS_NORM1000, 8+320, 30+240);
        expect(t, "window back norm", concat(a, concat(a, istr(a, m.x), ","), istr(a, m.y)),
               "500,500");
    }

    // The helper's geometry, and back
    {
        DeskShot s = deskshotparse(a, "8 30 640 480 320 240 0x0001007a\n");
        expect(t, "shot parse", concat(a, s.target, istr(a, s.ok)), "hwnd=0x0001007a1");
        expect(t, "shot str", deskshotstr(a, &s), "8 30 640 480 320 240 0x0001007a");
        s = deskshotparse(a, "0 0 1280 800 1280 800 0x0");
        expect(t, "shot screen", concat(a, s.target, istr(a, s.ok)), "screen1");
        expect(t, "shot screen str", deskshotstr(a, &s), "0 0 1280 800 1280 800 0x0");
        expect(t, "shot larger image", istr(a, deskshotparse(a, "0 0 10 10 20 20 0x0").ok), "0");
        expect(t, "shot short", istr(a, deskshotparse(a, "0 0 10 10 5").ok), "0");
        expect(t, "shot junk", istr(a, deskshotparse(a, "0 0 10 10 5 5 zz").ok), "0");
    }

    // Frames
    {
        Slice<Str> v = {};
        v = push(a, v, Str("click"));
        v = push(a, v, Str("screen"));
        v = push(a, v, Str("12"));
        Str f = deskframe(a, v);
        expect(t, "frame", f, Str{(u8 *)"\x10\0\0\0click\0screen\0" "12\0", 20});
        expect(t, "frame partial", istr(a, deskframelen(takehead(f, 19), 1<<20)), "0");
        expect(t, "frame whole", istr(a, deskframelen(f, 1<<20)), "20");
        expect(t, "frame too big", istr(a, deskframelen(f, 15)), "-1");
        expect(t, "frame no head", istr(a, deskframelen(takehead(f, 3), 1<<20)), "0");

        Str       reply = dkreply(a, 2, "no", "PNG");
        DeskReply r     = deskreply(reply);
        expect(t, "reply", concat(a, concat(a, istr(a, r.ok*10 + r.status), r.text), r.data),
               "12noPNG");
        expect(t, "reply short", istr(a, deskreply(takehead(reply, 8)).ok), "0");
        Str bad = clone(a, reply);
        bad[5] = 99;  // a text longer than the frame
        expect(t, "reply bad text", istr(a, deskreply(bad).ok), "0");
    }

    // Base64, PNG sizes, window names
    {
        expect(t, "base64 empty", base64(a, ""), "");
        expect(t, "base64 1", base64(a, "f"), "Zg==");
        expect(t, "base64 2", base64(a, "fo"), "Zm8=");
        expect(t, "base64 3", base64(a, "foo"), "Zm9v");
        expect(t, "base64 6", base64(a, "foobar"), "Zm9vYmFy");
        expect(t, "base64 high", base64(a, Str{(u8 *)"\xff\xfe\x00", 3}), "//4A");
        expect(t, "window id", deskwindow(a, " id=0x10, pid=7 "), "hwnd=0x10,pid=7");
        expect(t, "window title", deskwindow(a, "Notepad"), "Notepad");
        expect(t, "png not", istr(a, pngsize("GIF89a....................").w), "0");
    }

    // The tool's arguments
    {
        Args r = parseargs(a, TOOL_DESKTOP, "{\"action\":\"click\",\"x\":\"10\",\"y\":20.5}", 0);
        expect(t, "args click", concat(a, istr(a, r.issues.len), concat(a,
               istr(a, r.desk.action), istr(a, (i64)(r.desk.y*2)))), "0141");
        r = parseargs(a, TOOL_DESKTOP, "{\"action\":\"click\",\"x\":1}", 0);
        expect(t, "args no y", r.issues.len ? r.issues[0] : Str(), "x, y: the point is required");
        r = parseargs(a, TOOL_DESKTOP, "{\"action\":\"fly\"}", 0);
        expect(t, "args action", istr(a, startswith(r.issues[0],
               "action: unknown action \"fly\", expected one of: screenshot,")), "1");
        r = parseargs(a, TOOL_DESKTOP, "{\"action\":\"left_click\",\"x\":1,\"y\":2}", 0);
        expect(t, "args alias", istr(a, r.desk.action*10 + r.issues.len), "10");
        r = parseargs(a, TOOL_DESKTOP, "{\"action\":\"scroll\",\"x\":1,\"y\":2}", 0);
        expect(t, "args scroll", r.issues.len ? r.issues[0] : Str(),
               "amount: must be 1 to 100 notches up, or -1 to -100 down");
        r = parseargs(a, TOOL_DESKTOP, "{\"action\":\"wait\",\"window\":\"x\",\"seconds\":31}", 0);
        expect(t, "args wait", r.issues.len ? r.issues[0] : Str(), "seconds: must be 1 to 30");
        r = parseargs(a, TOOL_DESKTOP, "{\"action\":\"run\",\"program\":\"notepad\","
                      "\"args\":[\"a b\",\"c\"]}", 0);
        expect(t, "args run", concat(a, istr(a, r.issues.len), concat(a, r.desk.args[0],
               r.desk.args[1])), "0a bc");
        r = parseargs(a, TOOL_DESKTOP, "{\"action\":\"type\",\"text\":\"a\\u0000b\"}", 0);
        expect(t, "args null", istr(a, r.issues.len), "1");
        r = parseargs(a, TOOL_DESKTOP, "{\"action\":\"key\",\"keys\":\"  \"}", 0);
        expect(t, "args keys", r.issues.len ? r.issues[0] : Str(),
               "keys: at least one key is required");
    }

    // The schema: off by default, and the points described per model
    {
        ToolEnv te = {};
        te.deny = TOOL_OFFMASK;
        Str off = tooljson(a, &te);
        expect(t, "desktop off", istr(a, find(off, "\"desktop\"")), "-1");
        te.deny = 0;
        JsonDoc d = jsonparse(a, tooljson(a, &te));
        Json *desk = 0;
        for (Json *k = d.root->child; k; k = k->next) {
            Json *f = jsonget(k, "function");
            desk = jsonstr(jsonget(f, "name"))=="desktop" ? f : desk;
        }
        expect(t, "desktop schema", istr(a, desk!=0), "1");
        Str desc = jsonstr(jsonget(desk, "description"));
        expect(t, "desktop pixels", istr(a, find(desc, "in pixels of its image")>0), "1");
        Json *props = jsonget(jsonget(desk, "parameters"), "properties");
        expect(t, "desktop params", istr(a, jsonlen(props)), "13");
        expect(t, "desktop actions", istr(a, jsonlen(jsonget(jsonget(props, "action"), "enum"))),
               istr(a, DESK_COUNT));
        te.coords = COORDS_NORM1000;
        Str norm = tooljson(a, &te);
        expect(t, "desktop norm1000", istr(a, find(norm, "thousandths")>0), "1");
        Str sys = systemprompt(a, "/w", &te);
        expect(t, "desktop guideline", istr(a, find(sys, "desktop tool")>0), "1");
        expect(t, "computer alias", istr(a, find(unknowntool(a, "computer", 0),
               "Did you mean \"desktop\"")>0), "1");
    }

    // Screenshots in requests: Chat Completions after the run of tool
    // results, as a user message; Anthropic in the result itself
    {
        ModelConf mc = {};
        mc.name = "m";
        Block calls[2] = {};
        calls[0].type = BLOCK_CALL;
        calls[0].id   = "c1";
        calls[0].name = "desktop";
        calls[0].args = "{\"action\":\"screenshot\"}";
        calls[1].type = BLOCK_CALL;
        calls[1].id   = "c2";
        calls[1].name = "read";
        calls[1].args = "{\"path\":\"x\"}";
        Msg msgs[5] = {};
        msgs[0].role   = ROLE_USER;
        msgs[0].text   = "look";
        msgs[1].role   = ROLE_ASSISTANT;
        msgs[1].blocks = {calls, 2, 2};
        msgs[2].role   = ROLE_TOOL;
        msgs[2].callid = "c1";
        msgs[2].text   = "Took a screenshot";
        msgs[2].image  = "out/c1.png";
        msgs[2].imgb64 = "QUJD";
        msgs[3].role   = ROLE_TOOL;
        msgs[3].callid = "c2";
        msgs[3].text   = "data";
        msgs[4].role   = ROLE_USER;
        msgs[4].text   = "next";
        ChatReq r = {};
        r.mc      = &mc;
        r.history = {msgs, 5, 5};
        Arena tmp = *a;
        tmp.beg  += (tmp.end - tmp.beg)/2;
        Str body = encodechat(a, tmp, &r);
        expect(t, "openai image",
               istr(a, find(body, "{\"role\":\"tool\",\"tool_call_id\":\"c2\","
                    "\"content\":\"data\"},{\"role\":\"user\",\"content\":[{\"type\":"
                    "\"text\",\"text\":\"The screenshot of tool call c1:\"},{\"type\":"
                    "\"image_url\",\"image_url\":{\"url\":\"data:image/png;base64,QUJD\"}}]},"
                    "{\"role\":\"user\",\"content\":\"next\"}") > 0), "1");
        r.history = {msgs, 4, 4};
        body = encodechat(a, tmp, &r);
        expect(t, "openai image last", istr(a, find(body, "base64,QUJD\"}}]}]") > 0), "1");
        msgs[2].imgb64 = {};
        body = encodechat(a, tmp, &r);
        expect(t, "openai image left out", istr(a, find(body, "image_url")), "-1");

        msgs[2].imgb64 = "QUJD";
        r.history = {msgs, 5, 5};
        Str an = anencode(a, tmp, &r);
        expect(t, "anthropic image", istr(a, find(an, "{\"type\":\"tool_result\","
               "\"tool_use_id\":\"c1\",\"content\":[{\"type\":\"text\",\"text\":"
               "\"Took a screenshot\"},{\"type\":\"image\",\"source\":{\"type\":"
               "\"base64\",\"media_type\":\"image/png\",\"data\":\"QUJD\"}}]}") > 0), "1");
        expect(t, "anthropic shape", anshape(a, an),
               "user:text* | assistant:tool_use,tool_use | user:tool_result,"
               "tool_result,text*");
    }

    // The helper's core: fitting, scaling, PNG, selection, chords
    {
        DkSize s = dkfit(2560, 1440, 1568, 1150000);
        expect(t, "fit", concat(a, concat(a, istr(a, s.w), "x"), istr(a, s.h)), "1429x804");
        expect(t, "fit pixels", istr(a, (i64)s.w*s.h <= 1150000), "1");
        s = dkfit(1280, 800, 1568, 1150000);
        expect(t, "fit as is", concat(a, concat(a, istr(a, s.w), "x"), istr(a, s.h)), "1280x800");
        s = dkfit(4000, 1000, 1000, 0);
        expect(t, "fit edge", concat(a, concat(a, istr(a, s.w), "x"), istr(a, s.h)), "1000x250");
        s = dkfit(3, 1, 0, 1);
        expect(t, "fit tiny", concat(a, concat(a, istr(a, s.w), "x"), istr(a, s.h)), "1x1");

        u8 src[4*4*2] = {};
        for (i32 i = 0; i < 8; i++) {
            src[i*4] = (u8)(i*10);  // blue: 0 10 20 30 / 40 50 60 70
        }
        u8 *half = dkscale(a, src, 4, 2, 2, 1);
        expect(t, "scale", concat(a, istr(a, half[0]), concat(a, ",", istr(a, half[4]))),
               "25,45");

        u8  px[2*2*4] = {1, 2, 3, 0, 4, 5, 6, 0, 7, 8, 9, 0, 10, 11, 12, 0};
        Str png = dkpng(a, px, 2, 2);
        DeskSize ps = pngsize(png);
        expect(t, "png size", concat(a, istr(a, ps.w), istr(a, ps.h)), "22");
        expect(t, "png end", istr(a, endswith(png, Str{(u8 *)"IEND\xae\x42\x60\x82", 8})), "1");
        // Every chunk's CRC holds
        b32 ok = 1;
        for (iz i = 8; ok && i+12 <= png.len;) {
            u32 len = (u32)png[i]<<24 | (u32)png[i+1]<<16 | (u32)png[i+2]<<8 | png[i+3];
            u32 crc = dkcrc32(0, png.data+i+4, len+4);
            u8 *c   = png.data + i + 8 + len;
            ok = crc == ((u32)c[0]<<24 | (u32)c[1]<<16 | (u32)c[2]<<8 | c[3]);
            i += 12 + len;
        }
        expect(t, "png crcs", istr(a, ok), "1");

        Dk *dk = newdk(a);
        DkSel sel = dkparsesel("pid=7,class=notepad");
        expect(t, "sel", concat(a, istr(a, sel.pid), sel.cls), "7notepad");
        sel = dkparsesel("Untitled");
        expect(t, "sel title", sel.title, "Untitled");
        sel = dkparsesel("hwnd=0x10");
        expect(t, "sel hwnd", istr(a, (i64)sel.id), "16");
        sel = dkparsesel("pid=x");
        expect(t, "sel bad as title", sel.title, "pid=x");

        DkChord k = dkparsechord(dk, "ctrl+shift+a");
        expect(t, "chord", concat(a, istr(a, k.ok*10 + k.len), istr(a, k.codes[2])), "1397");
        k = dkparsechord(dk, "ctrl+!");
        expect(t, "chord shift", concat(a, istr(a, k.len), istr(a, k.codes[1])), "3258");
        k = dkparsechord(dk, "ctrl+nope");
        expect(t, "chord bad", istr(a, k.ok), "0");
        k = dkparsechord(dk, "ctrl++");
        expect(t, "chord plus", istr(a, k.ok*10 + k.len), "12");
    }

    // The helper serving: hello, a list, a shot, input in window
    // coordinates, failures, all as replies
    {
        Dk *dk = newdk(a);
        expect(t, "serve hello", dkround(a, dk, "hello"), "0|prospero-desktop 1 fake\n");
        expect(t, "serve list", dkround(a, dk, "list|pid=7"),
               "0|0x00000010\t7\t100,50\t640x480\tNotepad\tUntitled - Notepad\n"
               "0x00000020\t7\t300,200\t200x100\t#32768\t\n");
        Str data = {};
        expect(t, "serve shot", dkround(a, dk, "shot|-s|320|0|pid=7", &data),
               "0|0 0 640 480 320 240 0x00000010\n");
        DeskSize ps = pngsize(data);
        expect(t, "serve shot png", concat(a, istr(a, ps.w), concat(a, "x", istr(a, ps.h))),
               "320x240");
        expect(t, "serve shot screen", dkround(a, dk, "shot|screen|10|10|20|20"),
               "0|10 10 20 20 20 20 0x0\n");
        dklog(dk);
        expect(t, "serve click", dkround(a, dk, "click|hwnd=0x10|5|6"), "0|");
        expect(t, "serve click input", dklog(dk),
               "raise 0x0010\nmove 105,56\nleft down\nleft up\n");
        expect(t, "serve dclick", dkround(a, dk, "dclick|screen|1|2"), "0|");
        expect(t, "serve dclick input", dklog(dk),
               "move 1,2\nleft down\nleft up\nleft down\nleft up\n");
        expect(t, "serve type", dkround(a, dk, "type|Notepad|h\xc3\xa9\n"), "0|");
        expect(t, "serve type input", dklog(dk), "raise 0x0010\ntext h\xc3\xa9\n\n");
        expect(t, "serve key", dkround(a, dk, "key|active|ctrl+s|esc"), "0|");
        expect(t, "serve key input", dklog(dk), "raise 0x0010\nkeys 256 115\nkeys 260\n");
        expect(t, "serve wheel", dkround(a, dk, "wheel|screen|3|4|-2"), "0|");
        expect(t, "serve wheel input", dklog(dk), "move 3,4\nwheel -2,0\n");
        expect(t, "serve nomatch", dkround(a, dk, "click|Calc|1|1"),
               "2|no window matches 'Calc'");
        expect(t, "serve ambiguous", istr(a, startswith(dkround(a, dk, "raise|pid=7,title="),
               "0|")), "1");  // the popup is skipped
        expect(t, "serve bad number", dkround(a, dk, "click|screen|1|x"), "1|invalid number 'x'");
        expect(t, "serve missing", dkround(a, dk, "click|screen|1"), "1|missing argument: 'y'");
        expect(t, "serve unknown", dkround(a, dk, "fly"), "1|unknown command 'fly'");
        expect(t, "serve bad chord", dkround(a, dk, "key|screen|ctrl+zz"), "1|unknown key 'ctrl+zz'");
        expect(t, "serve wait", dkround(a, dk, "wait|-t|0.5|Calc"),
               "3|timed out waiting for 'Calc'");
        expect(t, "serve wait found", dkround(a, dk, "wait|Notepad"),
               "0|0x00000010\t7\t100,50\t640x480\tNotepad\tUntitled - Notepad\n");
        expect(t, "serve wait gone", dkround(a, dk, "wait|-gone|Calc"), "0|");
        dklog(dk);
        expect(t, "serve run", dkround(a, dk, "run|notepad.exe|a b"), "0|4242\n");
        expect(t, "serve run log", dklog(dk), "run notepad.exe a b\n");
        expect(t, "serve close", dkround(a, dk, "close|Notepad"), "0|");
        expect(t, "serve close log", dklog(dk), "close Untitled - Notepad\n");
        dklog(dk);
        dkround(a, dk, "drag|screen|0|0|10|0");
        Str drag = dklog(dk);
        expect(t, "serve drag", concat(a, takehead(drag, 23), taketail(drag, 18)),
               "move 0,0\nleft down\nmovemove 10,0\nleft up\n");

        // The serve loop over standard input, to its end
        Str one = dkrequest(a, "hello");
        Str two = dkrequest(a, "list|Program");
        dk->stdin_ = concat(a, one, two);
        expect(t, "serve loop", istr(a, dkserve(dk, *a)), "0");
        Str out = finish(&dk->out[1]);
        iz  n   = deskframelen(out, 1<<20);
        DeskReply r1 = deskreply(takehead(out, n));
        DeskReply r2 = deskreply(cuthead(out, n));
        expect(t, "serve loop replies", concat(a, r1.text, r2.text),
               "prospero-desktop 1 fake\n0x00000030\t9\t0,0\t1280x800\tProgman\t"
               "Program Manager\n");
        dk->stdin_ = Str{(u8 *)"\xff\xff\xff\x7f", 4};
        expect(t, "serve too long", istr(a, dkserve(dk, *a)), "1");
    }
}
