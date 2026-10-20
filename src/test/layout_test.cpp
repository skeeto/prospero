// Layout tests with a fixed-advance font: every character is 10 units
// wide (headings 20), lines are 20 tall with the baseline at 16.

static f32 fixedadvance(void *, i32 style, i32)
{
    return style&STYLE_HMASK ? 20 : 10;
}

static FontMetrics fixedfont(void *, i32 style)
{
    FontMetrics f = {};
    f.lineh  = style&STYLE_HMASK ? 40 : 20;
    f.ascent = style&STYLE_HMASK ? 32 : 16;
    return f;
}

static Measure fixedmeasure()
{
    Measure m = {};
    m.advance = fixedadvance;
    m.font    = fixedfont;
    return m;
}

// Render a block layout as lines: "y:x[text]x[text]..." per line, with
// breaks shown as "/" and tabs as ">".
static Str layoutshow(Arena *a, Doc *d, Placed *p)
{
    Buf          b(a, 256);
    BlockLayout *bl     = p->bl;
    Piece       *pieces = d->pieces.data + d->blocks[p->block].piece0;
    for (iz i = 0; i < bl->lines.len; i++) {
        Line *l = &bl->lines[i];
        print(&b, (i64)(l->y + p->y));
        putbyte(&b, ':');
        for (i32 k = 0; k < l->nrun; k++) {
            Run *r = &bl->runs[l->run0+k];
            print(&b, (i64)r->x);
            putbyte(&b, '[');
            Str t = span(pieces[r->piece].text.data+r->b0,
                         pieces[r->piece].text.data+r->b1);
            if (r->flags & RUN_NODRAW) {
                putbyte(&b, t.len && t[0]=='\t' ? '>' : '/');
            } else {
                print(&b, t);
            }
            putbyte(&b, ']');
        }
        putbyte(&b, ' ');
    }
    return finish(&b);
}

static Str msgshow(Arena *a, Str src, f32 width)
{
    Measure      m     = fixedmeasure();
    LayoutCache *cache = 0;
    Doc         *d     = parsedoc(a, src, 0);
    MsgLayout    ml    = layoutmsg(&cache, a, &m, d, width, 1);
    Buf          b(a, 256);
    for (iz i = 0; i < ml.blocks.len; i++) {
        if (i) print(&b, "| ");
        print(&b, layoutshow(a, d, &ml.blocks[i]));
    }
    print(&b, "h=");
    print(&b, (i64)ml.h);
    return finish(&b);
}

static void test_layout(Test *t)
{
    Arena perm = t->perm;

    struct { Str name, src; f32 width; Str want; } cases[] = {
        {"layout.fits", "hello world", 200, "0:0[hello world] h=20"},
        {"layout.wrap", "aaa bbb ccc", 75,
         "0:0[aaa bbb ] 20:0[ccc] h=40"},
        {"layout.hang", "aaa  bbb", 40, "0:0[aaa  ] 20:0[bbb] h=40"},
        {"layout.long", "abcdefghij", 40,
         "0:0[abcd] 20:0[efgh] 40:0[ij] h=60"},
        {"layout.styles", "aa **bb** cc", 50,
         "0:0[aa ]30[bb]50[ ] 20:0[cc] h=40"},
        {"layout.wordstyles", "a **bc**d e", 40,
         "0:0[a ] 20:0[bc]20[d ] 40:0[e] h=60"},
        {"layout.hard", "ab\\\ncd", 200, "0:0[ab]20[/] 20:0[cd] h=40"},
        {"layout.soft", "ab\ncd", 200, "0:0[ab]20[/]30[cd] h=20"},
        {"layout.softwrap", "ab\ncd", 40, "0:0[ab]20[/] 20:0[cd] h=40"},
        {"layout.code", "```\na\tb\nlonger line\n```", 100,
         "8:8[a]18[>]48[b]58[/] 28:8[longer l] 48:8[ine]38[/] h=76"},
        {"layout.heading", "# Hi\n\nyo", 200,
         "0:0[Hi] | 52:0[yo] h=72"},
        {"layout.list", "- a\n- b", 200, "0:32[a] | 23:32[b] h=43"},
        {"layout.table", "| a | b |\n|---|---|\n| 1 | 2 |", 200,
         "8:8[a] 8:68[b] 44:8[1] 44:68[2] h=72"},
        {"layout.tableshrink", "| aaaa bbbb | c |\n|---|---|\n| 1 | 2 |", 100,
         "8:8[aaaa ] 28:8[bbbb] 8:71[c] 64:8[1] 64:71[2] h=92"},
        {"layout.empty", "", 200, "h=0"},
    };
    for (iz i = 0; i < countof(cases); i++) {
        Arena a = perm;
        expect(t, cases[i].name, msgshow(&a, cases[i].src, cases[i].width),
               cases[i].want);
    }

    // List markers sit left of the text, numbered from the list's start
    {
        Arena        a     = perm;
        Measure      m     = fixedmeasure();
        LayoutCache *cache = 0;
        Doc         *d     = parsedoc(&a, "3. x\n4. y", 0);
        MsgLayout    ml    = layoutmsg(&cache, &a, &m, d, 200, 1);
        Buf b(&a, 64);
        for (iz i = 0; i < ml.blocks.len; i++) {
            BlockLayout *bl = ml.blocks[i].bl;
            for (iz k = 0; k < bl->decos.len; k++) {
                Deco *dc = &bl->decos[k];
                if (dc->kind != DECO_MARKER) continue;
                print(&b, dc->text);
                putbyte(&b, '@');
                print(&b, (i64)dc->x);
                putbyte(&b, ' ');
            }
        }
        expect(t, "layout.markers", finish(&b), "3.@4 4.@4 ");
    }

    // Hit testing maps every offset's x back to that offset
    {
        Arena        a     = perm;
        Measure      m     = fixedmeasure();
        LayoutCache *cache = 0;
        Str          src   = "one two three four five six seven";
        Doc         *d     = parsedoc(&a, src, 0);
        MsgLayout    ml    = layoutmsg(&cache, &a, &m, d, 100, 1);
        Placed      *p     = &ml.blocks[0];
        Piece       *pcs   = d->pieces.data;
        Str bad = {};
        for (iz li = 0; li < p->bl->lines.len; li++) {
            Line *l = &p->bl->lines[li];
            for (i32 k = 0; k < l->nrun; k++) {
                Run *r = &p->bl->runs[l->run0+k];
                for (i32 off = runbeg(pcs, r); off < runend(pcs, r); off++) {
                    f32 x   = runx(&m, pcs, r, off);
                    i32 got = msghit(&m, d, &ml, x+1, l->y+5);
                    if (got != off) {
                        bad = concat(&a, istr(&a, off), concat(&a, "->", istr(&a, got)));
                    }
                }
            }
        }
        expect(t, "layout.roundtrip", bad, "");

        // Left of a line is its start; right of it, its end
        expect(t, "layout.hitleft", istr(&a, msghit(&m, d, &ml, -50, 25)), "8");
        expect(t, "layout.hitright", istr(&a, msghit(&m, d, &ml, 900, 5)), "8");
        expect(t, "layout.hitabove", istr(&a, msghit(&m, d, &ml, 14, -30)), "1");
        expect(t, "layout.hitbelow", istr(&a, msghit(&m, d, &ml, 900, 500)),
               istr(&a, src.len));
        // Halfway into a character rounds to the nearer edge
        expect(t, "layout.hitmid", istr(&a, msghit(&m, d, &ml, 14, 5)), "1");
        expect(t, "layout.hitmid2", istr(&a, msghit(&m, d, &ml, 16, 5)), "2");
    }

    // Hits in code land on source offsets, tabs included
    {
        Arena        a     = perm;
        Measure      m     = fixedmeasure();
        LayoutCache *cache = 0;
        Str          src   = "```\na\tb\n```";
        Doc         *d     = parsedoc(&a, src, 0);
        MsgLayout    ml    = layoutmsg(&cache, &a, &m, d, 200, 1);
        expect(t, "layout.hittab", istr(&a, msghit(&m, d, &ml, 20, 10)), "5");
        expect(t, "layout.hitb", istr(&a, msghit(&m, d, &ml, 46, 10)), "6");
    }

    // The cache: one layout per (content, width), surviving re-parses
    {
        Arena        a     = perm;
        Measure      m     = fixedmeasure();
        LayoutCache *cache = 0;
        Doc         *d1    = parsedoc(&a, "stable\n\ntail", 0);
        Doc         *d2    = parsedoc(&a, "stable\n\ntail grows", 0);
        MsgLayout    l1    = layoutmsg(&cache, &a, &m, d1, 300, 1);
        MsgLayout    l2    = layoutmsg(&cache, &a, &m, d2, 300, 1);
        MsgLayout    l3    = layoutmsg(&cache, &a, &m, d2, 200, 1);
        MsgLayout    l4    = layoutmsg(&cache, &a, &m, d2, 300, 2);
        expect(t, "layout.reuse", istr(&a, l1.blocks[0].bl == l2.blocks[0].bl), "1");
        expect(t, "layout.tailnew", istr(&a, l1.blocks[1].bl != l2.blocks[1].bl), "1");
        expect(t, "layout.width", istr(&a, l2.blocks[0].bl != l3.blocks[0].bl), "1");
        expect(t, "layout.scale", istr(&a, l2.blocks[0].bl != l4.blocks[0].bl), "1");
    }

    // Two tables with the same first cell are not confused
    {
        Arena        a     = perm;
        Measure      m     = fixedmeasure();
        LayoutCache *cache = 0;
        Doc *d1 = parsedoc(&a, "| a | b |\n|---|---|\n| 1 | 2 |", 0);
        Doc *d2 = parsedoc(&a, "| a | b |\n|---|---|\n| 1 | 222222 |", 0);
        MsgLayout l1 = layoutmsg(&cache, &a, &m, d1, 400, 1);
        MsgLayout l2 = layoutmsg(&cache, &a, &m, d2, 400, 1);
        expect(t, "layout.tablekey", istr(&a, l1.blocks[0].bl != l2.blocks[0].bl), "1");
    }

    // Pathological widths: nothing fits, yet every character is placed
    {
        Arena a = perm;
        Str   s = msgshow(&a, "ab cd", 1);
        expect(t, "layout.narrow", s, "0:0[a] 20:0[b ] 40:0[c] 60:0[d] h=80");
    }
}
