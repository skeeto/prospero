// Thumbnail tests: a screenshot's thumbnail box

static void test_thumb(Test *t)
{
    Arena *a = &t->perm;

    // A screenshot's thumbnail: in its box, aspect kept, never enlarged
    {
        View     v = {};
        ViewTool tl = {};
        ViewMsg  m = {};
        v.thumbw   = 320;
        v.thumbh   = 200;
        v.thumbpad = 6;
        m.role  = VIEW_TOOL;
        m.tool  = &tl;
        tl.image = "out/c1.png";
        tl.imgw  = 1280;
        tl.imgh  = 1024;
        Thumb th = msgthumb(&v, &m);
        expect(t, "thumb tall", concat(a, istr(a, (i64)th.w), istr(a, (i64)th.h)), "250200");
        tl.imgw = 100;
        tl.imgh = 40;
        th = msgthumb(&v, &m);
        expect(t, "thumb small", concat(a, istr(a, (i64)th.w), istr(a, (i64)th.h)), "10040");
        expect(t, "thumb foot", istr(a, (i64)msgfoot(&v, &m)), "46");
        m.collapsed = 1;
        expect(t, "thumb collapsed", istr(a, (i64)msgthumb(&v, &m).h), "0");
    }
}
