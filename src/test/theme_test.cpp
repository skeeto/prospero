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
        P(codetext, codebg), P(text, headerbg), P(text, sel),
        P(text, toolbg), P(dim, toolbg), P(summary, toolbg),
        P(text, askbg), P(dim, askbg), P(summary, askbg),
        P(ok, panel), P(bad, panel), P(amber, panel), P(active, panel),
        P(ok, toolbg), P(bad, toolbg), P(amber, toolbg), P(active, toolbg),
        P(ok, askbg), P(bad, askbg), P(amber, askbg),
        P(text, selrow), P(dim, selrow), P(ok, selrow), P(bad, selrow),
        P(amber, selrow), P(active, selrow),
        #undef P
    };
    Palette *pals[] = {&themedark, &themelight};
    for (iz p = 0; p < countof(pals); p++) {
        Palette *pal  = pals[p];
        Str      name = pal->dark ? Str("theme.dark ") : Str("theme.light ");
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
}
