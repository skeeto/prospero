// Font tests: the embedded fonts' character maps, through both cmap
// formats, and prose's chain of fallbacks against the symbols models
// commonly write, each of which some font in it must have.

static void test_fonts(Test *t)
{
    Arena a = t->perm;

    // Each format the files use: Roboto and JetBrains Mono have format
    // 12 subtables, the subsets only format 4
    expect(t, "fonts.roboto.a", istr(&a, fontcovers(fontroboto(), 'a')), "1");
    expect(t, "fonts.roboto.arrow", istr(&a, fontcovers(fontroboto(), 0x2192)), "0");
    expect(t, "fonts.mono.arrow", istr(&a, fontcovers(fontmono(), 0x2192)), "1");
    expect(t, "fonts.mono.check", istr(&a, fontcovers(fontmono(), 0x2714)), "0");
    expect(t, "fonts.symbols.check", istr(&a, fontcovers(fontsymbols(), 0x2714)), "1");
    expect(t, "fonts.symbols.a", istr(&a, fontcovers(fontsymbols(), 'a')), "0");
    expect(t, "fonts.math.return", istr(&a, fontcovers(fontmath(), 0x21b5)), "1");
    expect(t, "fonts.math.beyond", istr(&a, fontcovers(fontmath(), 0x1f600)), "0");
    expect(t, "fonts.none", istr(&a, fontcovers({}, 'a')), "0");
    expect(t, "fonts.cut", istr(&a, fontcovers({(u8 *)robotottf, 400}, 'a')), "0");

    // Every symbol on the list, some font in prose's chain having it
    Buf missing(&a, 256);
    Str s = fontsymbolsused;
    i32 n = 0;
    for (iz i = 0; i < s.len;) {
        i32 cp = utf8decode(s, &i);
        if (cp == ' ') {
            continue;
        }
        n++;
        if (!fontprose(cp)) {
            print(&missing, "U+");
            printhex(&missing, (u64)cp, 4);
            putbyte(&missing, ' ');
        }
    }
    expect(t, "fonts.symbols.missing", finish(&missing), "");
    expect(t, "fonts.symbols.count", istr(&a, n > 60), "1");

    // The GUI's own: the branch, ahead and behind, deletions, a cut
    // (ui.cpp drawbranch, drawchanges)
    i32 const gui[] = {0xe0a0, 0x2191, 0x2193, 0x2212, 0x2026, 0x2192, 0xb7};
    Buf lacking(&a, 64);
    for (i32 cp : gui) {
        if (!fontprose(cp)) {
            print(&lacking, "U+");
            printhex(&lacking, (u64)cp, 4);
            putbyte(&lacking, ' ');
        }
    }
    expect(t, "fonts.gui.missing", finish(&lacking), "");
    expect(t, "fonts.gui.branch", istr(&a, fontcovers(fontmono(), 0xe0a0)), "1");

    // Buttons' icons: each in Codicons, in its range, and in no font
    // merged ahead of it, which would draw its own glyph instead
    Buf bad(&a, 256);
    i32 icons = 0;
    for (iz i = 0; i < fonticonsused.len;) {
        i32 cp = utf8decode(fonticonsused, &i);
        icons++;
        if (!fontcovers(fonticons(), cp) || cp<0xea60 || cp>0xf102 || fontprose(cp)) {
            print(&bad, "U+");
            printhex(&bad, (u64)cp, 4);
            putbyte(&bad, ' ');
        }
    }
    expect(t, "fonts.icons.bad", finish(&bad), "");

    // What draws nothing draws as U+FEFF, which each face's first font
    // has (zero-width there); none of it is a glyph of its own anywhere
    expect(t, "fonts.invisible.feff",
           istr(&a, fontcovers(fontroboto(), 0xfeff) && fontcovers(fontmono(), 0xfeff)), "1");
    i32 drawn = 0;
    for (auto r : fontinvisible) {
        for (i32 cp = r[0]; cp <= r[1]; cp++) {
            drawn += fontprose(cp);
        }
    }
    expect(t, "fonts.invisible.drawn", istr(&a, drawn), "0");
    expect(t, "fonts.icons.count", istr(&a, icons), "22");

    // Emoji: those models write in Twemoji's subset, and what a font
    // ahead of it has left out of it, to stay text
    Buf noemoji(&a, 256);
    i32 emoji = 0;
    for (iz i = 0; i < fontemojiused.len;) {
        i32 cp = utf8decode(fontemojiused, &i);
        if (cp == ' ') {
            continue;
        }
        emoji++;
        if (!fontcovers(fontemoji(), cp)) {
            print(&noemoji, "U+");
            printhex(&noemoji, (u64)cp, 4);
            putbyte(&noemoji, ' ');
        }
    }
    expect(t, "fonts.emoji.missing", finish(&noemoji), "");
    expect(t, "fonts.emoji.count", istr(&a, emoji > 50), "1");
    expect(t, "fonts.emoji.text", istr(&a, fontcovers(fontemoji(), 0x2714) ||
                                           fontcovers(fontemoji(), 0x26a0) ||
                                           fontcovers(fontemoji(), 0x2605)), "0");
    expect(t, "fonts.emoji.prose", istr(&a, fontprose(0x2705) && fontprose(0x1f680)), "1");

    // A collection (.ttc) of one face, Roboto behind a "ttcf" header,
    // its tables' offsets moved with it: the same face, found by index
    Str  rob = fontroboto();
    iz   hdr = 16;
    Str  ttc = {allocbytes(&a, hdr+rob.len), hdr+rob.len};
    u8 const head[] = {'t','t','c','f', 0,1,0,0, 0,0,0,1, 0,0,0,(u8)hdr};
    __builtin_memcpy(ttc.data, head, (uz)hdr);
    __builtin_memcpy(ttc.data+hdr, rob.data, (uz)rob.len);
    for (u32 i = 0; i < fontbe16(rob, 4); i++) {
        iz  at  = hdr + 12 + 16*(iz)i + 8;
        u32 off = fontbe32(ttc, at) + (u32)hdr;
        ttc.data[at]   = (u8)(off>>24);
        ttc.data[at+1] = (u8)(off>>16);
        ttc.data[at+2] = (u8)(off>>8);
        ttc.data[at+3] = (u8)off;
    }
    expect(t, "fonts.ttc.faces", istr(&a, fontfaces(ttc)), "1");
    expect(t, "fonts.ttc.dir", istr(&a, fontface(ttc, 0)), "16");
    expect(t, "fonts.ttc.past", istr(&a, fontface(ttc, 1)), "-1");
    expect(t, "fonts.ttc.a", istr(&a, cmapcovers(fonttable(ttc, 16, 0x636d6170), 'a')), "1");
    expect(t, "fonts.ttc.psname", fontpsname(&a, ttc, 16), "Roboto-Medium");
    expect(t, "fonts.psname", fontpsname(&a, rob, 0), "Roboto-Medium");
    expect(t, "fonts.plain.index", istr(&a, fontface(rob, 1)), "-1");

    // None of the embedded fonts has CJK, which the system's fill in
    u32 cjk = fontcjkmask(rob, 0) | fontcjkmask(fontmono(), 0) |
              fontcjkmask(fontsymbols(), 0) | fontcjkmask(fontmath(), 0) |
              fontcjkmask(fontemoji(), 0);
    expect(t, "fonts.cjk.embedded", istr(&a, cjk), "0");
    expect(t, "fonts.drawable", istr(&a, fontdrawable(rob, 0) && fontdrawable(ttc, 16)), "1");
    expect(t, "fonts.drawable.none", istr(&a, fontdrawable({}, 0)), "0");
    // Roboto's hhea: 1900 above, 500 below, 2048 units per em
    expect(t, "fonts.lineems", istr(&a, (i64)(fontlineems(rob, 0)*1000 + 0.5f)), "1172");
    expect(t, "fonts.lineems.ttc", istr(&a, fontlineems(ttc, 16)==fontlineems(rob, 0)), "1");
}
