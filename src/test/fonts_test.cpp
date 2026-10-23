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
}
