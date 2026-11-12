// Theme tests: the setting's names, and every palette's text colors
// against the backgrounds they are drawn on, by the WCAG's contrast
// ratio, which must reach 4.5 (AA for normal text).

// An sRGB channel as linear light: x^2.4, past the linear toe, as the
// fifth root of x^12 by Newton's method, which needs no math library.
static f64 srgblinear(u32 c)
{
    f64 x = (f64)c / 255;
    if (x <= 0.04045) {
        return x / 12.92;
    }
    f64 y  = (x + 0.055) / 1.055;
    f64 y4 = y*y*y*y;
    f64 a  = y4*y4*y4;
    f64 r  = y;  // above the root, so the iteration descends to it
    for (i32 i = 0; i < 64; i++) {
        r = (4*r + a/(r*r*r*r)) / 5;
    }
    return r;
}

static f64 luminance(u32 c)
{
    return 0.2126*srgblinear(c&255) + 0.7152*srgblinear(c>>8&255) +
           0.0722*srgblinear(c>>16&255);
}

static f64 contrast(u32 a, u32 b)
{
    f64 x = luminance(a) + 0.05;
    f64 y = luminance(b) + 0.05;
    return x>y ? x/y : y/x;
}

#ifdef __clang__
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wc23-extensions"
#endif
static u8 const gruvini[] = {
    #embed "../../assets/themes/gruvbox.ini"
};
static u8 const monoini[] = {
    #embed "../../assets/themes/monocream.ini"
};
#ifdef __clang__
#  pragma clang diagnostic pop
#endif

static void test_theme(Test *t)
{
    Arena a = t->perm;

    expect(t, "theme.parse.light", istr(&a, themeparse("light")), istr(&a, THEME_LIGHT));
    expect(t, "theme.parse.system", istr(&a, themeparse("system")), istr(&a, THEME_SYSTEM));
    expect(t, "theme.parse.bad", istr(&a, themeparse("Dark")), "-1");
    expect(t, "theme.system.dark", istr(&a, themepalette(THEME_SYSTEM, 1)->dark), "1");
    expect(t, "theme.system.light", istr(&a, themepalette(THEME_SYSTEM, 0)->dark), "0");
    expect(t, "theme.forced", istr(&a, themepalette(THEME_LIGHT, 1)->dark), "0");

    // The ratio of a known pair: white on black is 21
    Buf k(&a, 16);
    printfloat(&k, contrast(0xffffffff, 0xff000000), 2);
    expect(t, "theme.contrast.max", finish(&k), "21");

    struct { Str name; iz fg, bg; } pairs[] = {
        #define P(f, b) {#f " on " #b, __builtin_offsetof(Palette, f), __builtin_offsetof(Palette, b)}
        P(paneltext, panel), P(paneldim, panel), P(paneltext, frame),
        P(paneldim, frame), P(paneltext, bar), P(paneltext, selrow),
        P(text, panel), P(dim, panel), P(reasoning, panel), P(link, panel),
        P(code, panel), P(marker, panel), P(stale, panel),
        P(text, userbg), P(dim, userbg), P(link, userbg), P(code, userbg),
        P(codetext, codebg), P(text, headerbg), P(seltext, sel),
        P(text, toolbg), P(dim, toolbg), P(summary, toolbg),
        P(code, toolbg), P(link, toolbg), P(summary, panel),
        P(text, askbg), P(dim, askbg), P(summary, askbg),
        P(ok, panel), P(bad, panel), P(amber, panel), P(active, panel),
        P(ok, toolbg), P(bad, toolbg), P(amber, toolbg), P(active, toolbg),
        P(ok, askbg), P(bad, askbg), P(amber, askbg),
        P(text, selrow), P(dim, selrow), P(ok, selrow), P(bad, selrow),
        P(amber, selrow), P(active, selrow),
        // A row under the mouse, or pressed, shows what a selected one
        // does; a tab under the mouse, or a button, shows paneltext
        P(text, rowhover), P(dim, rowhover), P(ok, rowhover),
        P(bad, rowhover), P(amber, rowhover), P(active, rowhover),
        P(text, rowpress), P(dim, rowpress), P(ok, rowpress),
        P(bad, rowpress), P(amber, rowpress), P(active, rowpress),
        P(paneltext, rowhover), P(paneltext, btn), P(paneltext, btnhover),
        P(paneltext, btnpress), P(paneltext, btnon), P(paneltext, btnonhover),
        P(paneldim, rowhover), P(termfg, termbg),
        // Tabs, the bars behind them, the menu bar, and a field under the
        // mouse or active, all under paneltext
        P(paneltext, tab), P(paneltext, tabsel), P(paneltext, tabbar),
        P(paneltext, tabbaron), P(paneltext, menubar),
        P(paneltext, framehover), P(paneltext, frameactive),
        #undef P
    };
    // The shipped Gruvbox, included as a user's configuration would, held
    // to the built-in themes' contrast
    CfgEnv v = newcfgenv(&a);
    os_write(v.os, a, "/c/themes/gruvbox.ini", Str{(u8 *)gruvini, countof(gruvini)});
    os_write(v.os, a, "/c/prospero.ini",
             "include = themes/gruvbox.ini\n[ui]\ntheme = gruvbox\n");
    Config *c = newconfig(v.os, &a, v.scratch, "/c/prospero.ini", 0);
    expect(t, "theme.gruvbox.diags", inidiags(&a, c->ini), "");
    Palette gruvbox = themebuild(cfgget(c, "theme", "gruvbox", "base", 0),
                                 [&](Str key) { return cfgget(c, "theme", "gruvbox", key, 0); });
    expect(t, "theme.gruvbox.panel", istr(&a, gruvbox.panel), istr(&a, rgb(29, 32, 33)));
    expect(t, "theme.gruvbox.blue", istr(&a, gruvbox.ansi[4]), istr(&a, rgb(69, 133, 136)));
    cfgfree(c);

    // The shipped monocream, included as a user's configuration would. Its
    // colors are the author's own, so the checks below fail on it until the
    // palette is revised with them, and they are named for it either way.
    CfgEnv mv = newcfgenv(&a);
    os_write(mv.os, a, "/c/themes/monocream.ini", Str{(u8 *)monoini, countof(monoini)});
    os_write(mv.os, a, "/c/prospero.ini",
             "include = themes/monocream.ini\n[ui]\ntheme = monocream\n");
    Config *mc = newconfig(mv.os, &a, mv.scratch, "/c/prospero.ini", 0);
    expect(t, "theme.monocream.diags", inidiags(&a, mc->ini), "");
    Palette monocream = themebuild(cfgget(mc, "theme", "monocream", "base", 0),
                                   [&](Str key) { return cfgget(mc, "theme", "monocream", key, 0); });
    expect(t, "theme.monocream.panel", istr(&a, monocream.panel), istr(&a, rgb(25, 25, 0)));
    expect(t, "theme.monocream.blue", istr(&a, monocream.ansi[4]), istr(&a, rgb(200, 200, 120)));
    cfgfree(mc);

    Palette *pals[]  = {&themedark, &themelight, &gruvbox, &monocream};
    Str      names[] = {"theme.dark ", "theme.light ", "theme.gruvbox ", "theme.monocream "};
    for (iz p = 0; p < countof(pals); p++) {
        Palette *pal  = pals[p];
        Str      name = names[p];
        for (iz i = 0; i < countof(pairs); i++) {
            u32 fg = *(u32 *)((byte *)pal + pairs[i].fg);
            u32 bg = *(u32 *)((byte *)pal + pairs[i].bg);
            f64 r  = contrast(fg, bg);
            Buf b(&a, 16);
            if (r < 4.5) {
                printfloat(&b, r, 2);
            } else {
                print(&b, "AA");
            }
            expect(t, concat(&a, name, pairs[i].name), finish(&b), "AA");
        }
        // A toggle that is on stands out from one that is off (no fill, on
        // panel), from a plain button, and from one off under the mouse
        {
            u32 offs[] = {pal->panel, pal->btn, pal->rowhover};
            for (iz i = 0; i < countof(offs); i++) {
                f64 r1 = contrast(pal->btnon, offs[i]);
                f64 r2 = contrast(pal->btnonhover, offs[i]);
                f64 r  = r1<r2 ? r1 : r2;
                Buf b(&a, 16);
                if (r < 1.5) {
                    printfloat(&b, r, 2);
                } else {
                    print(&b, "apart");
                }
                expect(t, concat(&a, concat(&a, name, "btnon against "), istr(&a, i)),
                       finish(&b), "apart");
            }
        }
        // A terminal's selection stands out from its background
        {
            f64 r = contrast(pal->sel, pal->termbg);
            Buf b(&a, 16);
            if (r < 1.5) {
                printfloat(&b, r, 2);
            } else {
                print(&b, "apart");
            }
            expect(t, concat(&a, name, "sel against termbg"), finish(&b), "apart");
        }
        for (i32 i = HL_PLAIN+1; i < HL_COUNT; i++) {
            f64 r = contrast(pal->hl[i], pal->codebg);
            Buf b(&a, 16);
            if (r < 4.5) {
                printfloat(&b, r, 2);
            } else {
                print(&b, "AA");
            }
            Buf n(&a, 64);
            print(&n, name);
            print(&n, "highlight class ");
            print(&n, (i64)i);
            print(&n, " on codebg");
            expect(t, finish(&n), finish(&b), "AA");
        }
    }

    // [theme NAME] keys: config.cpp's, in order, each a palette color of
    // its own, all of them but the unused hl[HL_PLAIN] and dark
    {
        iz n = 0;
        for (iz i = 0; i < countof(cfgschema); i++) {
            CfgKey *k = &cfgschema[i];
            if (k->kind=="theme" && k->key!="base") {
                Str want = n<countof(themekeys) ? themekeys[n].key : Str("(none)");
                expect(t, "theme.keys.order", k->key, want);
                n++;
            }
        }
        expect(t, "theme.keys.count", istr(&a, n), istr(&a, countof(themekeys)));
        iz colors = (iz)(sizeof(Palette) - sizeof(b32))/(iz)sizeof(u32) - 1;
        expect(t, "theme.keys.all", istr(&a, countof(themekeys)), istr(&a, colors));
        for (iz i = 0; i < countof(themekeys); i++) {
            for (iz k = 0; k < i; k++) {
                if (themekeys[i].off == themekeys[k].off) {
                    expect(t, "theme.keys.twice", themekeys[i].key, themekeys[k].key);
                }
            }
            if (themekeys[i].off==__builtin_offsetof(Palette, hl) ||
                    themekeys[i].off < (iz)sizeof(b32)) {
                expect(t, "theme.keys.unused", themekeys[i].key, "");
            }
        }
    }

    // Building: the base whole, colors over it, shades that follow a
    // button's color, and what is not a color left alone
    {
        auto none = [](Str) { return Str{}; };
        Palette l = themebuild("light", none);
        Palette d = themebuild("sepia", none);
        expect(t, "theme.build.light", istr(&a, !__builtin_memcmp(&l, &themelight, sizeof(l))), "1");
        expect(t, "theme.build.dark", istr(&a, !__builtin_memcmp(&d, &themedark, sizeof(d))), "1");

        Palette p = themebuild("dark", [](Str key) {
            return key=="link"      ? Str("#abc")    :
                   key=="button"    ? Str("#405060") :
                   key=="toggle_on" ? Str("#123456") :
                   key=="toggle_on_hover" ? Str("#654321") :
                   key=="text"      ? Str("blue")    : Str{};
        });
        expect(t, "theme.build.link", istr(&a, p.link), istr(&a, rgb(0xaa, 0xbb, 0xcc)));
        expect(t, "theme.build.text", istr(&a, p.text), istr(&a, themedark.text));
        expect(t, "theme.build.btn", istr(&a, p.btn), istr(&a, rgb(0x40, 0x50, 0x60)));
        // dark's button is (36,69,110), its hover (46,88,140): +10,+19,+30
        expect(t, "theme.build.btnhover", istr(&a, p.btnhover),
               istr(&a, rgb(0x40+10, 0x50+19, 0x60+30)));
        // and pressed (30,62,104): -6,-7,-6
        expect(t, "theme.build.btnpress", istr(&a, p.btnpress),
               istr(&a, rgb(0x40-6, 0x50-7, 0x60-6)));
        expect(t, "theme.build.onhover", istr(&a, p.btnonhover), istr(&a, rgb(0x65, 0x43, 0x21)));
        // A field and a scrollbar shade as a button does; unchanged, not
        expect(t, "theme.build.framehover", istr(&a, p.framehover), istr(&a, themedark.framehover));
        Palette f = themebuild("dark", [](Str key) {
            return key=="field" ? Str("#203040") : key=="field_active" ? Str("#010203") : Str{};
        });
        // dark's field is (30,47,74), its hover (48,64,79): +18,+17,+5
        expect(t, "theme.build.fieldhover", istr(&a, f.framehover),
               istr(&a, rgb(0x20+18, 0x30+17, 0x40+5)));
        expect(t, "theme.build.fieldactive", istr(&a, f.frameactive), istr(&a, rgb(1, 2, 3)));

        expect(t, "theme.shift.clamp", istr(&a, themeshift(rgb(250, 5, 128), rgb(0, 10, 0), rgb(10, 0, 0))),
               istr(&a, rgb(255, 0, 128)));
        expect(t, "theme.put.unknown", istr(&a, themeput(&p, "nope", "#fff")), "0");
    }
}
