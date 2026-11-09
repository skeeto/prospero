// PDF encoding tests: the encoding tables, glyph names to code points,
// text strings in each of their forms, the one emitter of extracted
// text, and the standard fonts' widths.

// A code point as hex, at least four digits.
static void printcp(Buf *b, u32 v)
{
    printhex(b, v, v>0xfffff ? 6 : v>0xffff ? 5 : 4);
}

// A glyph name's code points, as hex, space separated.
static Str pdfglyphhex(Arena *a, Str name)
{
    u32 cp[PDF_UNIMAX];
    i32 n = pdfglyphname(name, cp);
    Buf b(a, 64);
    for (i32 i = 0; i < n; i++) {
        if (i) putbyte(&b, ' ');
        printcp(&b, cp[i]);
    }
    return finish(&b);
}

// What pdfputcp makes of each code point, concatenated.
static Str pdfputs(Arena *a, i64 *cps, iz n)
{
    Buf b(a, 64);
    for (iz i = 0; i < n; i++) {
        pdfputcp(&b, cps[i]);
    }
    return finish(&b);
}

static Str pdfhex4(Arena *a, u32 v)
{
    Buf b(a, 8);
    printhex(&b, v, 4);
    return finish(&b);
}

static void test_pdfenc(Test *t)
{
    Arena a = t->perm;

    // Each table at a few telling codes
    struct { char const *name; u16 const *tab; i32 code; u32 want; } spots[] = {
        {"std A",          PDF_STDENC,   0x41, 0x0041},
        {"std quoteright", PDF_STDENC,   0x27, 0x2019},
        {"std quoteleft",  PDF_STDENC,   0x60, 0x2018},
        {"std fi",         PDF_STDENC,   0xae, 0xfb01},
        {"std fl",         PDF_STDENC,   0xaf, 0xfb02},
        {"std emdash",     PDF_STDENC,   0xd0, 0x2014},
        {"std Lslash",     PDF_STDENC,   0xe8, 0x0141},
        {"std germandbls", PDF_STDENC,   0xfb, 0x00df},
        {"std hole",       PDF_STDENC,   0x80, 0},
        {"std control",    PDF_STDENC,   0x0a, 0},
        {"mac Adieresis",  PDF_MACROMAN, 0x80, 0x00c4},
        {"mac dagger",     PDF_MACROMAN, 0xa0, 0x2020},
        {"mac notequal",   PDF_MACROMAN, 0xad, 0x2260},
        {"mac currency",   PDF_MACROMAN, 0xdb, 0x00a4},
        {"mac apple",      PDF_MACROMAN, 0xf0, 0},
        {"mac caron",      PDF_MACROMAN, 0xff, 0x02c7},
        {"win Euro",       PDF_WINANSI,  0x80, 0x20ac},
        {"win hole 81",    PDF_WINANSI,  0x81, 0x2022},
        {"win hole 9d",    PDF_WINANSI,  0x9d, 0x2022},
        {"win del",        PDF_WINANSI,  0x7f, 0x2022},
        {"win quotesingle",PDF_WINANSI,  0x27, 0x0027},
        {"win eacute",     PDF_WINANSI,  0xe9, 0x00e9},
        {"doc breve",      PDF_PDFDOC,   0x18, 0x02d8},
        {"doc tilde",      PDF_PDFDOC,   0x1f, 0x02dc},
        {"doc bullet",     PDF_PDFDOC,   0x80, 0x2022},
        {"doc minus",      PDF_PDFDOC,   0x8a, 0x2212},
        {"doc fi",         PDF_PDFDOC,   0x93, 0xfb01},
        {"doc zcaron",     PDF_PDFDOC,   0x9e, 0x017e},
        {"doc 9f",         PDF_PDFDOC,   0x9f, 0},
        {"doc Euro",       PDF_PDFDOC,   0xa0, 0x20ac},
        {"doc ad",         PDF_PDFDOC,   0xad, 0},
        {"doc tab",        PDF_PDFDOC,   0x09, 0x0009},
        {"sym alpha",      PDF_SYMBOL,   0x61, 0x03b1},
        {"sym Delta",      PDF_SYMBOL,   0x44, 0x0394},
        {"sym Omega",      PDF_SYMBOL,   0x57, 0x03a9},
        {"sym mu",         PDF_SYMBOL,   0x6d, 0x03bc},
        {"sym forall",     PDF_SYMBOL,   0x22, 0x2200},
        {"sym minus",      PDF_SYMBOL,   0x2d, 0x2212},
        {"sym infinity",   PDF_SYMBOL,   0xa5, 0x221e},
        {"sym integral",   PDF_SYMBOL,   0xf2, 0x222b},
        {"sym Euro",       PDF_SYMBOL,   0xa0, 0x20ac},
        {"sym copyright",  PDF_SYMBOL,   0xe3, 0x00a9},
        {"sym radicalex",  PDF_SYMBOL,   0x60, 0},
        {"sym suchthat",   PDF_SYMBOL,   0x27, 0x220b},
        {"sym lozenge",    PDF_SYMBOL,   0xe0, 0x25ca},
        {"sym parenlt",    PDF_SYMBOL,   0xe6, 0x239b},
        {"sym braceex",    PDF_SYMBOL,   0xef, 0x23aa},
        {"sym integralex", PDF_SYMBOL,   0xf4, 0x23ae},
        {"sym bracerb",    PDF_SYMBOL,   0xfe, 0x23ad},
        {"sym arrowvert",  PDF_SYMBOL,   0xbd, 0x23d0},
        {"sym apple",      PDF_SYMBOL,   0xf0, 0},
        {"zapf a1",        PDF_ZAPF,     0x21, 0x2701},
        {"zapf phone",     PDF_ZAPF,     0x25, 0x260e},
        {"zapf circle",    PDF_ZAPF,     0x6c, 0x25cf},
        {"zapf one",       PDF_ZAPF,     0xac, 0x2460},
        {"zapf arrow",     PDF_ZAPF,     0xd5, 0x2192},
        {"zapf hole",      PDF_ZAPF,     0x80, 0},
        {"cork grave",     PDF_CORK,     0x00, 0x0060},
        {"cork ff",        PDF_CORK,     0x1b, 0xfb00},
        {"cork ffl",       PDF_CORK,     0x1f, 0xfb04},
        {"cork pthzero",   PDF_CORK,     0x18, 0},
        {"cork dotlessj",  PDF_CORK,     0x1a, 0x0237},
        {"cork quoteright",PDF_CORK,     0x27, 0x2019},
        {"cork Abreve",    PDF_CORK,     0x80, 0x0102},
        {"cork SS",        PDF_CORK,     0xdf, 0},
        {"cork germandbls",PDF_CORK,     0xff, 0x00df},
    };
    for (iz i = 0; i < countof(spots); i++) {
        Str name = concat(&a, "pdfenc.table ", Str((u8 *)spots[i].name,
                          __builtin_strlen(spots[i].name)));
        expect(t, name, pdfhex4(&a, spots[i].tab[spots[i].code]),
               pdfhex4(&a, spots[i].want));
    }

    // WinAnsi is Latin-1 plus web1252 above the controls, but for holes
    Buf diff(&a, 256);
    for (i32 c = 0x20; c < 0x100; c++) {
        u32 want = c>=0x80 && c<0xa0 ? (u32)web1252[c-0x80] : (u32)c;
        if (want==0x81 || want==0x8d || want==0x8f || want==0x90 ||
            want==0x9d || want==0x7f) {
            want = 0x2022;
        }
        if (PDF_WINANSI[c] != want) {
            printhex(&diff, (u32)c, 2);
            putbyte(&diff, ' ');
        }
    }
    expect(t, "pdfenc.winansi web1252", finish(&diff), "");

    // Encodings by name, and the built-in ones of Symbol and Zapf
    expect(t, "pdfenc.encoding std", istr(&a, pdfencoding("StandardEncoding")==PDF_STDENC), "1");
    expect(t, "pdfenc.encoding win", istr(&a, pdfencoding("WinAnsiEncoding")==PDF_WINANSI), "1");
    expect(t, "pdfenc.encoding mac", istr(&a, pdfencoding("MacRomanEncoding")==PDF_MACROMAN), "1");
    expect(t, "pdfenc.encoding expert", istr(&a, !pdfencoding("MacExpertEncoding")), "1");
    expect(t, "pdfenc.builtin symbol", istr(&a, pdfbuiltin("Symbol")==PDF_SYMBOL), "1");
    expect(t, "pdfenc.builtin subset", istr(&a, pdfbuiltin("ABCDEF+ZapfDingbats")==PDF_ZAPF), "1");
    expect(t, "pdfenc.builtin other", istr(&a, !pdfbuiltin("Helvetica")), "1");

    // The table is sorted, so the search finds every name
    Buf unsorted(&a, 256);
    for (iz i = 1; i < countof(PDF_AGL); i++) {
        Str p = {(u8 *)PDF_AGLNAMES+PDF_AGL[i-1].off, PDF_AGL[i-1].len};
        Str q = {(u8 *)PDF_AGLNAMES+PDF_AGL[i].off,   PDF_AGL[i].len};
        if (compare(p, q) >= 0) {
            print(&unsorted, q);
            putbyte(&unsorted, ' ');
        }
        u32 cp[PDF_UNIMAX];
        if (pdfglyphname(q, cp)<1 || cp[0]!=PDF_AGL[i].cp[0]) {
            print(&unsorted, q);
            putbyte(&unsorted, '?');
        }
    }
    expect(t, "pdfenc.agl sorted", finish(&unsorted), "");

    // Glyph names
    struct { char const *name, *want; } names[] = {
        {"A",              "0041"},
        {"space",          "0020"},
        {"quoteright",     "2019"},
        {"quotedblleft",   "201c"},
        {"endash",         "2013"},
        {"emdash",         "2014"},
        {"Euro",           "20ac"},
        {"eacute",         "00e9"},
        {"Lslash",         "0141"},
        {"lslash",         "0142"},
        {"dotlessi",       "0131"},
        {"dotlessj",       "0237"},
        {"Zcaron",         "017d"},
        {"gcommaaccent",   "0123"},
        {"alpha",          "03b1"},
        {"Omega",          "03a9"},
        {"Delta",          "0394"},
        {"suchthat",       "220b"},
        {"parenlefttp",    "239b"},
        {"bracerightbt",   "23ad"},
        {"epsilon1",       "03f5"},
        {"theta1",         "03d1"},
        {"pi1",            "03d6"},
        {"rho1",           "03f1"},
        {"sigma1",         "03c2"},
        {"phi1",           "03c6"},
        {"minus",          "2212"},
        {"multiply",       "00d7"},
        {"plusminus",      "00b1"},
        {"arrowright",     "2192"},
        {"arrowdblright",  "21d2"},
        {"element",        "2208"},
        {"infinity",       "221e"},
        {"lessequal",      "2264"},
        {"summation",      "2211"},
        {"integral",       "222b"},
        {"partialdiff",    "2202"},
        {"ff",             "0066 0066"},
        {"fi",             "0066 0069"},
        {"fl",             "0066 006c"},
        {"ffi",            "0066 0066 0069"},
        {"ffl",            "0066 0066 006c"},
        {"Acyrillic",      "0410"},
        {"afii10017",      "0410"},
        {"afii10022",      "0415"},
        {"afii10023",      "0401"},
        {"afii10024",      "0416"},
        {"afii10049",      "042f"},
        {"afii10050",      "0490"},
        {"afii10065",      "0430"},
        {"afii10071",      "0451"},
        {"afii10097",      "044f"},
        {"afii10098",      "0491"},
        {"afii10064",      ""},
        {"afii1001x",      ""},
        {"uni0041",        "0041"},
        {"uni00410042",    "0041 0042"},
        {"uni00e9",        "00e9"},
        {"uniD83D",        ""},
        {"uni0041D83D",    ""},
        {"uni004",         ""},
        {"uni00410",       ""},
        {"uni004G",        ""},
        {"u0041",          "0041"},
        {"u1F600",         "1f600"},
        {"u10FFFF",        "10ffff"},
        {"u110000",        ""},
        {"uD800",          ""},
        {"u041",           ""},
        {"u0041000",       ""},
        {"a.sc",           "0061"},
        {"one.oldstyle",   "0031"},
        {"uni0041.alt",    "0041"},
        {"f_f_i",          "0066 0066 0069"},
        {"f_f_i.liga",     "0066 0066 0069"},
        {"T_h",            "0054 0068"},
        {"a_bogus_b",      "0061 0062"},
        {"uni0066_uni0069","0066 0069"},
        {".notdef",        ""},
        {"",               ""},
        {"g123",           ""},
        {"cid65",          ""},
        {"glyph7",         ""},
        {"bogus",          ""},
        {"a_a_a_a_a_a_a_a_a_a", "0061 0061 0061 0061 0061 0061 0061 0061"},
        {"ffi_ffi_ffi",    "0066 0066 0069 0066 0066 0069 0066 0066"},
    };
    for (iz i = 0; i < countof(names); i++) {
        Str name = Str((u8 *)names[i].name, __builtin_strlen(names[i].name));
        Str want = Str((u8 *)names[i].want, __builtin_strlen(names[i].want));
        expect(t, concat(&a, "pdfenc.glyph ", name), pdfglyphhex(&a, name), want);
    }

    // Type3 numeric names, only when the number is the code
    expect(t, "pdfenc.cork a27",  pdfhex4(&a, pdfcork("a27", 27)), "fb00");
    expect(t, "pdfenc.cork a28",  pdfhex4(&a, pdfcork("a28", 28)), "fb01");
    expect(t, "pdfenc.cork a65",  pdfhex4(&a, pdfcork("a65", 65)), "0041");
    expect(t, "pdfenc.cork cid65",pdfhex4(&a, pdfcork("cid65", 65)), "0000");
    expect(t, "pdfenc.cork g1",   pdfhex4(&a, pdfcork("g1", 1)), "0000");
    expect(t, "pdfenc.cork a123", pdfhex4(&a, pdfcork("a123", 203)), "0000");
    expect(t, "pdfenc.cork g123", pdfhex4(&a, pdfcork("g123", 203)), "0000");
    expect(t, "pdfenc.cork a1000",pdfhex4(&a, pdfcork("a1000", 1000)), "0000");
    expect(t, "pdfenc.cork abcd1",pdfhex4(&a, pdfcork("abcd1", 1)), "0000");
    expect(t, "pdfenc.cork a",    pdfhex4(&a, pdfcork("a", 0)), "0000");
    expect(t, "pdfenc.cork 65",   pdfhex4(&a, pdfcork("65", 65)), "0000");
    expect(t, "pdfenc.cork a6x",  pdfhex4(&a, pdfcork("a6x", 6)), "0000");
    expect(t, "pdfenc.cork a300", pdfhex4(&a, pdfcork("a300", 300)), "0000");

    // UTF-16: pairs, lone halves, an odd last byte
    {
        Str s = "\xd8\x3e\xdd\x8a\x00\x41\xd8\x00\x00\x42\xdc\x00\x00";
        Buf b(&a, 64);
        for (iz i = 0; i < s.len;) {
            printcp(&b, (u32)pdfutf16(s, &i, 0));
            putbyte(&b, ' ');
        }
        expect(t, "pdfenc.utf16", finish(&b), "1f98a 0041 fffd 0042 fffd fffd ");
        Str le = "\x3e\xd8\x8a\xdd\x41\x00";
        Buf c(&a, 64);
        for (iz i = 0; i < le.len;) {
            printcp(&c, (u32)pdfutf16(le, &i, 1));
            putbyte(&c, ' ');
        }
        expect(t, "pdfenc.utf16 le", finish(&c), "1f98a 0041 ");
    }

    // Text strings
    struct { char const *name; Str raw; Str want; } strs[] = {
        {"ascii",     "Hello",                                 "Hello"},
        {"trim",      "  Hello \n",                            "Hello"},
        {"doc",       "\x93nd \x84 \x8dq\x8e \xa0",            "find \xe2\x80\x94 \xe2\x80\x9cq\xe2\x80\x9d \xe2\x82\xac"},
        {"doc breve", "a\x18" "b",                             "a\xcb\x98" "b"},
        {"doc latin1","caf\xe9",                               "caf\xc3\xa9"},
        {"doc hole",  "a\x9f" "b",                             "a\xef\xbf\xbd" "b"},
        {"doc ctl",   "a\tb\x01" "c",                          "a b c"},
        {"doc nul",   "a\0b",                          "ab"},
        {"be",        "\xfe\xff\x03\xa9\x00m",         "\xce\xa9m"},
        {"le",        "\xff\xfe\xa9\x03m\x00",         "\xce\xa9m"},
        {"be pair",   "\xfe\xff\xd8\x3e\xdd\x8a",      "\xf0\x9f\xa6\x8a"},
        {"be lone",   "\xfe\xff\xd8\x3e\x00x",         "\xef\xbf\xbdx"},
        {"be odd",    "\xfe\xff\x00x\x00",             "x\xef\xbf\xbd"},
        {"be esc",    "\xfe\xff\x00\x1b\x00" "e\x00" "n\x00\x1b\x00H\x00i", "Hi"},
        {"be esc2",   "\xfe\xff\x00H\x00\x1b\x00" "f\x00r\x00\x1b\x00i", "Hi"},
        {"be esc open","\xfe\xff\x00H\x00\x1b\x00" "e\x00" "n", "H en"},
        {"be esc lone","\xfe\xff\x00\x41\x00\x1b\x00\x42\x00\x43", "A BC"},
        {"be esc only","\xfe\xff\x00\x1b\x00\x41",   "A"},
        {"be esc far", "\xfe\xff\x00\x1b\x00" "a\x00" "b\x00" "c\x00" "d\x00" "e\x00\x1b\x00x",
                       "abcde x"},
        {"be esc 2b",  "\xfe\xff\x00\x1b" "en" "\x00\x1b\x00H\x00i", "Hi"},
        {"be esc 4b",  "\xfe\xff\x00\x1b" "enUS" "\x00\x1b\x00H\x00i", "Hi"},
        {"be esc esc", "\xfe\xff\x00H\x00\x1b\x00\x1b\x00i", "H  i"},
        {"le esc",     "\xff\xfe\x1b\x00" "en" "\x1b\x00" "H\x00", "H"},
        {"be hi+bmp",  "\xfe\xff\xd8\x3e\xe0\x00",   "\xef\xbf\xbd\xee\x80\x80"},
        {"be lone lo", "\xfe\xff\xdc\x00\x00x",       "\xef\xbf\xbdx"},
        {"be lo lo",   "\xfe\xff\xdc\x00\xdc\x00",   "\xef\xbf\xbd\xef\xbf\xbd"},
        {"le lone hi", "\xff\xfe\x3e\xd8x\x00",       "\xef\xbf\xbdx"},
        {"le lone lo", "\xff\xfe\x00\xdcx\x00",       "\xef\xbf\xbdx"},
        {"le hi+bmp",  "\xff\xfe\x3e\xd8\x00\xe0",   "\xef\xbf\xbd\xee\x80\x80"},
        {"be ctl",    "\xfe\xff\x00\x0a\x00H\x00\x0d", "H"},
        {"be lig",    "\xfe\xff\xfb\x03",              "ffi"},
        {"be bom only","\xfe\xff",                     ""},
        {"utf8",      "\xef\xbb\xbf" "caf\xc3\xa9",            "caf\xc3\xa9"},
        {"utf8 bad",  "\xef\xbb\xbf" "a\xc3(b",                "a\xef\xbf\xbd(b"},
        {"utf8 surr", "\xef\xbb\xbf\xed\xa0\x80",              "\xef\xbf\xbd"},
        {"utf8 esc",  "\xef\xbb\xbf\x1b" "de\x1b" "Ja",        "Ja"},
        {"utf8 esc4", "\xef\xbb\xbf\x1b" "deAT\x1b" "Ja",      "Ja"},
        {"utf8 esc lone","\xef\xbb\xbf" "a\x1b" "bcdefg",      "a bcdefg"},
        {"empty",     "",                                      ""},
    };
    for (iz i = 0; i < countof(strs); i++) {
        Str name = concat(&a, "pdfenc.textstr ", Str((u8 *)strs[i].name,
                          __builtin_strlen(strs[i].name)));
        expect(t, name, pdftextstr(&a, strs[i].raw), strs[i].want);
    }
    {
        // Costs exactly its length, and soft fails when that won't fit
        Arena small = carve(&a, 9);  // fits wants a byte to spare
        expect(t, "pdfenc.textstr exact", pdftextstr(&small, "\x93\x93\x93\x93"), "fifififi");
        expect(t, "pdfenc.textstr full", istr(&a, small.end-small.beg), "1");
        expect(t, "pdfenc.textstr nofit", pdftextstr(&small, "x"), "");
    }

    // The emitter
    {
        i64 cps[] = {'a', 0, 'b'};
        expect(t, "pdfenc.putcp nul", pdfputs(&a, cps, 3), "ab");
    }
    {
        i64 cps[] = {0x01, 0x0a, 0x1f, 0x7f, 0x85, 0xa0};
        expect(t, "pdfenc.putcp spaces", pdfputs(&a, cps, 6), "      ");
    }
    {
        i64 cps[] = {0xad};
        expect(t, "pdfenc.putcp shy", pdfputs(&a, cps, 1), "-");
    }
    {
        i64 cps[] = {0xd800, 0xdfff, 0x110000, -1, 0xfffe, 0xffff, 0x10ffff, I64MAX, I64MIN};
        Str rep = "\xef\xbf\xbd";
        Str want = {};
        for (i32 i = 0; i < 9; i++) want = concat(&a, want, rep);
        expect(t, "pdfenc.putcp invalid", pdfputs(&a, cps, 9), want);
    }
    {
        i64 cps[] = {0xfb00, 0xfb01, 0xfb02, 0xfb03, 0xfb04, 0xfb05, 0xfb06};
        expect(t, "pdfenc.putcp ligatures", pdfputs(&a, cps, 7), "fffiflffifflstst");
    }
    {
        i64 cps[] = {0xe9, 0x20ac, 0x1f98a, 0x10fffd, 0xfffd, 0xfb07};
        expect(t, "pdfenc.putcp valid", pdfputs(&a, cps, 6),
               "\xc3\xa9\xe2\x82\xac\xf0\x9f\xa6\x8a\xf4\x8f\xbf\xbd\xef\xbf\xbd\xef\xac\x87");
    }

    // Widths of the standard fonts without their own
    expect(t, "pdfenc.width helv space", istr(&a, pdfstdwidth("Helvetica", ' ')), "278");
    expect(t, "pdfenc.width helv A",     istr(&a, pdfstdwidth("Helvetica", 'A')), "667");
    expect(t, "pdfenc.width helvb A",    istr(&a, pdfstdwidth("Helvetica-Bold", 'A')), "722");
    expect(t, "pdfenc.width helvbo A",   istr(&a, pdfstdwidth("Helvetica-BoldOblique", 'A')), "722");
    expect(t, "pdfenc.width helvo A",    istr(&a, pdfstdwidth("Helvetica-Oblique", 'A')), "667");
    expect(t, "pdfenc.width arialb",     istr(&a, pdfstdwidth("Arial,Bold", 'A')), "722");
    expect(t, "pdfenc.width arialbmt",   istr(&a, pdfstdwidth("XYZABC+Arial-BoldMT", 'A')), "722");
    expect(t, "pdfenc.width timesb m",   istr(&a, pdfstdwidth("Times-Bold", 'm')), "833");
    expect(t, "pdfenc.width timesi a",   istr(&a, pdfstdwidth("Times-Italic", 'a')), "500");
    expect(t, "pdfenc.width timesi A",   istr(&a, pdfstdwidth("TimesNewRomanPS-ItalicMT", 'A')), "611");
    expect(t, "pdfenc.width timesbi m",  istr(&a, pdfstdwidth("Times-BoldItalic", 'm')), "778");
    expect(t, "pdfenc.width times a",    istr(&a, pdfstdwidth("Times-Roman", 'a')), "444");
    expect(t, "pdfenc.width helv quote", istr(&a, pdfstdwidth("Helvetica", '\'')), "191");
    expect(t, "pdfenc.width helv grave", istr(&a, pdfstdwidth("Helvetica", '`')), "333");
    expect(t, "pdfenc.width helv at",    istr(&a, pdfstdwidth("Helvetica", '@')), "1015");
    expect(t, "pdfenc.width helv tilde", istr(&a, pdfstdwidth("Helvetica", '~')), "584");
    expect(t, "pdfenc.width arial",      istr(&a, pdfstdwidth("ABCDEF+Arial", 'm')), "833");
    expect(t, "pdfenc.width times space",istr(&a, pdfstdwidth("Times-Roman", ' ')), "250");
    expect(t, "pdfenc.width times W",    istr(&a, pdfstdwidth("Times-Roman", 'W')), "944");
    expect(t, "pdfenc.width times quote",istr(&a, pdfstdwidth("Times-Roman", '\'')), "180");
    expect(t, "pdfenc.width courier",    istr(&a, pdfstdwidth("Courier-Oblique", 0xe9)), "600");
    expect(t, "pdfenc.width high",       istr(&a, pdfstdwidth("Helvetica", 0xe9)), "0");
    expect(t, "pdfenc.width other",      istr(&a, pdfstdwidth("Garamond", 'a')), "0");
    expect(t, "pdfenc.width badtag",     istr(&a, pdfstdwidth("abcdef+Times", 'a')), "0");
}
