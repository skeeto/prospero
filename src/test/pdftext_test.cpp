// PDF text tests: CMaps parsed and looked up, fonts, the content
// interpreter over pages written by pdfbuild, page markers and image
// placements, refusals that leave perm as it was, and reads cut short
// by too little work or memory.

// What a CMap makes of codes: each code split by its codespaces, as
// hex/length, then "=" its Unicode (hex code points) or "#" its CID.
static Str pdfcmapdump(Arena *a, Str cmap, Str codes, Str base = {})
{
    Buf     b(a, 1<<12);
    Arena   s = *a;
    Pdf     p = pdfstart(s, cmap, {});  // its budgets as a file of its size
    PdfCMap m = {};
    if (base.len) {
        pdfcmapnamed(&m, base);
    }
    pdfcmap(&p, &m, cmap);
    if (m.vertical) print(&b, "V ");
    for (iz i = 0; i < codes.len;) {
        i32 n    = 0;
        u32 code = pdfnextcode(&m, codes, &i, &n);
        u32 cps[PDF_MAPMAX];
        i32 k    = pdftounicode(&m, code, n, cps);
        printhex(&b, code, 2*n);
        putbyte(&b, '/');
        print(&b, (i64)n);
        if (k) {
            putbyte(&b, '=');
            for (i32 j = 0; j < k; j++) {
                if (j) putbyte(&b, ',');
                printhex(&b, cps[j], cps[j]>0xffff ? 5 : 4);
            }
        } else {
            putbyte(&b, '#');
            print(&b, (i64)pdfcid(&m, code, n));
        }
        putbyte(&b, ' ');
    }
    print(&b, "r");
    print(&b, m.nr);
    if (p.why.data) {
        print(&b, " ! ");
        print(&b, p.why);
    }
    assert(b.cap == 1<<12);  // or it grew over the reader
    return finish(&b);
}

// A document of extra objects 3.., then a page per content, sharing the
// resources res, each page's contents a stream.
static Str pdftdoc(Arena *a, Str res, Str *contents, i32 n, Str *extra = 0, i32 ne = 0,
                   Str pageextra = {}, Str catextra = {}, i32 flags = 0)
{
    i32  total = 2 + ne + 2*n;
    Str *objs  = alloc<Str>(a, total);
    Buf  kids(a, 256);
    for (i32 i = 0; i < n; i++) {
        print(&kids, (i64)(3 + ne + i));
        print(&kids, " 0 R ");
    }
    objs[0] = concat(a, concat(a, "<< /Type /Catalog /Pages 2 0 R ", catextra), " >>");
    objs[1] = concat(a, concat(a, concat(a, concat(a,
              "<< /Type /Pages /MediaBox [0 0 612 792] /Kids [", finish(&kids)),
              "] /Resources "), res), " >>");
    for (i32 i = 0; i < ne; i++) {
        objs[2+i] = extra[i];
    }
    for (i32 i = 0; i < n; i++) {
        Buf pg(a, 128);
        print(&pg, "<< /Type /Page /Parent 2 0 R ");
        print(&pg, pageextra);
        print(&pg, " /Contents ");
        print(&pg, (i64)(3 + ne + n + i));
        print(&pg, " 0 R >>");
        objs[2+ne+i]   = finish(&pg);
        objs[2+ne+n+i] = pdfbstream(a, "", contents[i]);
    }
    return pdfbuild(a, objs, total, flags);
}

static Str const PDFT_HELV = "<< /Font << /F1 << /Type /Font /Subtype /Type1 "
                             "/BaseFont /Helvetica >> >> >>";

// A read, its results in a block taken from a, its scratch the rest.
static PdfOut pdftread(Arena *a, Str pdf, PdfLimits lim = {}, iz room = 0)
{
    Arena  perm = carve(a, 1<<20);
    Arena  s    = *a;
    PdfReq q    = {};
    q.lim = lim;
    if (room) {
        s.end = s.beg + room;
    }
    return pdfread(&perm, s, pdf, &q);
}

// The text of one page drawn by content, less its marker line.
static Str pdftone(Arena *a, Str content, Str res = PDFT_HELV, Str *extra = 0, i32 ne = 0)
{
    PdfOut r = pdftread(a, pdftdoc(a, res, &content, 1, extra, ne));
    Str    s = r.text;
    if (startswith(s, "--- page 1 ---\n")) {
        s = cuthead(s, 15);
    }
    return r.why.data ? concat(a, concat(a, s, "! "), r.why) : s;
}

// Placements, as "id page line objnum stream inlineoff WxH label whole".
static Str pdfplaces(Arena *a, PdfOut r)
{
    Buf b(a, 1024);
    for (iz i = 0; i < r.places.len; i++) {
        PdfPlace *pl = r.places.data + i;
        print(&b, (i64)pl->id);
        putbyte(&b, ' ');
        print(&b, (i64)pl->page);
        putbyte(&b, ' ');
        print(&b, pl->line);
        putbyte(&b, ' ');
        print(&b, (i64)pl->objnum);
        putbyte(&b, ' ');
        print(&b, (i64)pl->stream);
        putbyte(&b, ' ');
        print(&b, pl->inlineoff);
        putbyte(&b, ' ');
        print(&b, pl->label);
        print(&b, pl->wholepage ? Str(" whole") : Str(""));
        print(&b, ";");
    }
    return finish(&b);
}

// The text up to and including the last marker's line before its end.
static Str pdfwholepages(PdfOut r)
{
    Str s = r.text;
    if (!r.pageline.len) return {};
    iz  last = r.pageline.data[r.pageline.len-1];
    iz  line = 1;
    for (iz i = 0; i < s.len; i++) {
        if (line == last) return takehead(s, i);
        line += s[i] == '\n';
    }
    return {};
}

static void testreadpdfmemo(Test *t);
static void testpdfattachnoimages(Test *t);
static void test_pdftext(Test *t)
{
    Arena a = t->perm;

    // CMaps: codespaces of mixed lengths split codes; bfchar to a name
    // and a surrogate pair; <0000> for one code maps nothing
    {
        Str cm = "/CIDInit /ProcSet findresource begin 12 dict begin begincmap\n"
                 "2 begincodespacerange <00> <7f> <8000> <ffff> endcodespacerange\n"
                 "4 beginbfchar <41> <0061> <8141> /fi <8142> <d83edd8a> <42> <0000> endbfchar\n"
                 "endcmap";
        expect(t, "pdftext.cmap bfchar", pdfcmapdump(&a, cm, "A\x81\x41\x81\x42" "B"),
               "41/1=0061 8141/2=0066,0069 8142/2=1f98a 42/1#0 r3");
    }
    // bfrange: offsets incrementing the last code point, a range from
    // <0000>, an array shorter than its range, a range crossing nothing
    {
        Str cm = "begincodespacerange <0000> <ffff> endcodespacerange\n"
                 "3 beginbfrange <0010> <0012> <00660066> <1000> <10ff> <0000>\n"
                 "<0020> <0025> [<0041> <> <00420043>] endbfrange";
        expect(t, "pdftext.cmap bfrange", pdfcmapdump(&a, cm,
               Str((u8 *)"\x00\x10\x00\x12\x10\x05\x00\x20\x00\x21\x00\x22\x00\x23", 14)),
               "0010/2=0066,0066 0012/2=0066,0068 1005/2=0005 0020/2=0041 "
               "0021/2#0 0022/2=0042,0043 0023/2#0 r3");
    }
    // cidchar and cidrange, a later entry winning an overlap; bad entries
    // (lengths unequal, lo past hi, a range of strings) dropped
    {
        Str cm = "/WMode 1 def\nbegincodespacerange <0000> <ffff> endcodespacerange\n"
                 "begincidrange <0100> <01ff> 1000 <0150> <0150> 7 <02> <0300> 5 "
                 "<0400> <03ff> 9 endcidrange begincidchar <0005> 77 endcidchar";
        expect(t, "pdftext.cmap cid", pdfcmapdump(&a, cm,
               Str((u8 *)"\x01\x00\x01\x50\x01\x51\x00\x05\x00\x06\x04\x00", 12)),
               "V 0100/2#1000 0150/2#7 0151/2#1081 0005/2#77 0006/2#0 0400/2#0 r4");
    }
    // usecmap of an Identity, and the predefined CMaps
    expect(t, "pdftext.cmap usecmap", pdfcmapdump(&a, "/Identity-V usecmap",
           Str((u8 *)"\x12\x34\x05", 3)), "V 1234/2#4660 05/1#5 r0");
    expect(t, "pdftext.cmap utf16", pdfcmapdump(&a, "", Str((u8 *)"\x00\x41\xd8\x3e\xdd\x8a", 6),
           "UniJIS-UTF16-H"), "0041/2#65 d83edd8a/4#3627998602 r0");
    expect(t, "pdftext.cmap legacy", pdfcmapdump(&a, "", Str((u8 *)"A\x82\xa0", 3), "90ms-RKSJ-H"),
           "41/1#65 82a0/2#33440 r0");
    // Ranges are capped, never expanded: one over every code is one entry
    {
        Buf b(&a, 1<<21);
        print(&b, "begincodespacerange <0000> <ffff> endcodespacerange beginbfchar\n");
        for (i32 i = 0; i < PDF_CMAPRANGES+10; i++) {
            print(&b, "<0001> <0041>\n");
        }
        print(&b, "endbfchar beginbfrange <0000> <ffff> <0000> endbfrange");
        Str r = pdfcmapdump(&a, finish(&b), Str((u8 *)"\x00\x01", 2));
        expect(t, "pdftext.cmap cap", r, "0001/2=0041 r1");  // the same code: one piece
    }

    // Fonts: Helvetica's own widths give gaps, so words set apart by
    // position read with spaces, and kerning reads without
    expect(t, "pdftext.helvetica", pdftone(&a,
           "BT /F1 12 Tf 72 700 Td (Hello) Tj 40 0 Td (world) Tj "
           "0 -14 Td [(Ke) 20 (rned) -400 (gap)] TJ ET"),
           "Hello world\nKerned gap\n");

    // An indirect font's widths count from its /FirstChar: a glyph a
    // whole em wide ends where the next is placed (once read as 0)
    {
        Str extra[] = {"<< /Type /Font /Subtype /Type1 /BaseFont /Wide /FirstChar 65 "
                       "/Widths [1000 1000] >>"};
        expect(t, "pdftext.firstchar", pdftone(&a,
               "BT /F1 12 Tf 72 700 Td (A) Tj 12 0 Td (B) Tj ET",
               "<< /Font << /F1 3 0 R >> >>", extra, countof(extra)), "AB\n");
    }

    // Differences over WinAnsi, names through the glyph list, and an
    // unknown name; Symbol's own encoding
    {
        Str res = "<< /Font << /F1 << /Type /Font /Subtype /Type1 /BaseFont /Times-Roman "
                  "/Encoding << /BaseEncoding /WinAnsiEncoding /Differences "
                  "[65 /alpha /fi /g123 /uni2192] >> >> "
                  "/F2 << /Type /Font /Subtype /Type1 /BaseFont /Symbol >> >> >>";
        expect(t, "pdftext.differences", pdftone(&a,
               "BT /F1 12 Tf 72 700 Td (ABCDE\\200) Tj /F2 12 Tf 0 -14 Td (abg) Tj ET", res),
               "\xce\xb1""fi\xef\xbf\xbd\xe2\x86\x92""E\xe2\x82\xac\n\xce\xb1\xce\xb2\xce\xb3\n"
               "[1 character on this page is in fonts with no Unicode mapping]\n");
    }

    // An embedded Type 1 font's own encoding, from its cleartext
    {
        Str prog = "%!PS-AdobeFont-1.0: CMR10\n/Encoding 256 array\n"
                   "0 1 255 {1 index exch /.notdef put} for\n"
                   "dup 12 /fi put\ndup 97 /a put\ndup 14 /ffi put\nreadonly def\n"
                   "currentdict end\ncurrentfile eexec\n\x8f\x9a garbage";
        Str extra[] = {
            "<< /Type /Font /Subtype /Type1 /BaseFont /ABCDEF+CMR10 /FontDescriptor 4 0 R "
                "/FirstChar 12 /LastChar 14 /Widths [556 0 833] >>",
            "<< /Type /FontDescriptor /FontName /ABCDEF+CMR10 /FontFile 5 0 R >>",
            pdfbstream(&a, concat(&a, "/Length1 ", istr(&a, find(prog, "eexec")+6)), prog),
        };
        expect(t, "pdftext.type1 builtin", pdftone(&a,
               "BT /F1 10 Tf 72 700 Td (\\014a\\015\\016a) Tj ET",
               "<< /Font << /F1 3 0 R >> >>", extra, countof(extra)),
               "fia\xef\xbf\xbd""ffia\n"
               "[1 character on this page is in fonts with no Unicode mapping]\n");

        // The same program, and a ToUnicode, each behind a filter: the
        // decoders' temporaries go in op, so neither decodes there
        Buf hex(&a, prog.len*2 + 1);
        for (iz i = 0; i < prog.len; i++) {
            printhex(&hex, prog[i], 2);
        }
        print(&hex, ">");
        Str prog16 = finish(&hex);
        Str tou    = "626567696e636f6465737061636572616e6765203c30303e203c6666"
                     "3e20656e64636f6465737061636572616e6765203120626567696e62"
                     "6663686172203c34313e203c303035413e20656e64626663686172>";
        Str fext[] = {
            "<< /Type /Font /Subtype /Type1 /BaseFont /ABCDEF+CMR10 /FontDescriptor 5 0 R "
                "/FirstChar 12 /LastChar 14 /Widths [556 0 833] >>",
            "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /ToUnicode 7 0 R >>",
            "<< /Type /FontDescriptor /FontName /ABCDEF+CMR10 /FontFile 6 0 R >>",
            pdfbstream(&a, concat(&a, "/Filter /ASCIIHexDecode /Length1 ",
                                  istr(&a, find(prog, "eexec")+6)), prog16),
            pdfbstream(&a, "/Filter /ASCIIHexDecode", tou),
        };
        expect(t, "pdftext.filtered fonts", pdftone(&a,
               "BT /F1 10 Tf 72 700 Td (\\014a) Tj /F2 10 Tf 20 0 Td (AB) Tj ET",
               "<< /Font << /F1 3 0 R /F2 4 0 R >> >>", fext, countof(fext)),
               "fia ZB\n");
    }

    // Type 3: ToUnicode wins; else names, and the T1 fallback for a
    // name whose number is its code, but not another's; its em from its
    // widths and matrix, so its gaps read as spaces
    {
        Str extra[] = {
            "<< /Type /Font /Subtype /Type3 /FontMatrix [0.01 0 0 0.01 0 0] "
                "/FontBBox [0 0 100 100] /CharProcs << >> /FirstChar 27 /LastChar 66 "
                "/Widths [50 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 "
                "0 0 0 0 50 50] /Encoding << /Differences [27 /a27 65 /g99 /A] >> >>",
        };
        expect(t, "pdftext.type3", pdftone(&a,
               "BT /F1 12 Tf 72 700 Td (\\033) Tj 20 0 Td (BA) Tj ET",
               "<< /Font << /F1 3 0 R >> >>", extra, countof(extra)),
               "ff A\xef\xbf\xbd\n"
               "[1 character on this page is in fonts with no Unicode mapping]\n");
    }

    // Type 0: Identity-H with ToUnicode and /W widths; one without a
    // ToUnicode reads as U+FFFD, noted
    {
        Str tou = "begincodespacerange <0000> <ffff> endcodespacerange "
                  "2 beginbfchar <0003> <0048> <0004> <0069> endbfchar";
        Str extra[] = {
            "<< /Type /Font /Subtype /Type0 /BaseFont /X /Encoding /Identity-H "
                "/DescendantFonts [5 0 R] /ToUnicode 6 0 R >>",
            "<< /Type /Font /Subtype /Type0 /BaseFont /Y /Encoding /Identity-H "
                "/DescendantFonts [5 0 R] >>",
            "<< /Type /Font /Subtype /CIDFontType2 /BaseFont /X /DW 1000 "
                "/W [3 [700 300]] >>",
            pdfbstream(&a, "", tou),
        };
        expect(t, "pdftext.type0", pdftone(&a,
               "BT /F1 10 Tf 72 700 Td <00030004> Tj 0 -12 Td /F2 10 Tf <00030004> Tj ET",
               "<< /Font << /F1 3 0 R /F2 4 0 R >> >>", extra, countof(extra)),
               "Hi\n\xef\xbf\xbd\n[2 characters on this page are in fonts with no "
               "Unicode mapping]\n");
    }

    // The interpreter: q/Q, cm (a NaN one dropped), TD/T*/'/", Tz, rise,
    // every render mode, and junk operands overflowing the stack
    expect(t, "pdftext.state", pdftone(&a,
           "q 2 0 0 2 0 0 cm BT /F1 6 Tf 36 350 Td (Big) Tj ET Q "
           "1 0 0 1 nan 0 cm 1e300 0 0 1 0 0 cm "
           "BT /F1 12 Tf 72 600 Td 0 -14 TD (one) Tj T* (two) Tj (three) ' 1 2 (four) \" "
           "3 Tr (hidden) ' ET "
           "1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 "
           "31 32 33 34 35 36 37 38 39 40 41 42 43 44 45 46 47 48 49 50 51 52 53 54 55 56 57 "
           "58 59 60 61 62 63 64 65 66 67 68 69 70 BT /F1 12 Tf 72 400 Td (last) Tj ET"),
           "Big\n\none\ntwo\nthree\nfour\nhidden\n\nlast\n");

    // Lines by baseline, a superscript joining its line, top to bottom,
    // each ordered along it whatever the drawing order
    expect(t, "pdftext.lines", pdftone(&a,
           "BT /F1 12 Tf 1 0 0 1 200 600 Tm (right) Tj 1 0 0 1 72 600 Tm (left) Tj "
           "/F1 7 Tf 1 0 0 1 88.5 605 Tm (1) Tj /F1 12 Tf 1 0 0 1 72 700 Tm (top) Tj ET"),
           "top\n\nleft1  right\n");

    // ActualText stands for the glyphs it covers
    expect(t, "pdftext.actualtext", pdftone(&a,
           "BT /F1 12 Tf 72 700 Td (The ) Tj /Span << /ActualText (word) >> BDC "
           "(\\267\\267\\267) Tj EMC ( here) Tj ET"),
           "The word here\n");
    // A spacing accent drawn over a letter, as TeX's \accent does, accent
    // first, is its combining mark; over a dotless i, a dotted one's
    {
        Str res = "<< /Font << /F1 << /Type /Font /Subtype /Type1 /BaseFont /Helvetica >> "
                  "/F2 << /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding "
                  "<< /Differences [1 /acute /dieresis /dotlessi] >> >> >> >>";
        expect(t, "pdftext.accent", pdftone(&a,
               "BT /F1 12 Tf 72 700 Td (caf) Tj ET BT /F2 12 Tf 89.346 700 Td <01> Tj ET "
               "BT /F1 12 Tf 88.008 700 Td (e) Tj ET "
               "BT /F1 12 Tf 72 680 Td (Na) Tj ET BT /F2 12 Tf 87.006 680 Td <02> Tj ET "
               "BT /F2 12 Tf 87.336 680 Td <03> Tj ET BT /F1 12 Tf 90.672 680 Td (ve) Tj ET "
               "BT /F2 12 Tf 72 660 Td (\001 \002) Tj ET", res),
               "cafe\xcc\x81\nNai\xcc\x88ve\n\xc2\xb4 \xc2\xa8\n");
    }

    // A negative size turns the glyphs and their way: still read in order
    expect(t, "pdftext.negative size", pdftone(&a,
           "BT /F1 -12 Tf 300 700 Td (Negative size) Tj ET"), "Negative size\n");

    // A tab is a space; a simple font's glyph with no Unicode is U+FFFD,
    // counted in the note
    {
        Str res = "<< /Font << /F1 << /Type /Font /Subtype /Type1 /BaseFont /Helvetica "
                  "/Encoding /WinAnsiEncoding >> /F2 << /Type /Font /Subtype /Type1 "
                  "/BaseFont /Helvetica /Encoding << /Differences [1 /g1 /g2] >> >> >> >>";
        expect(t, "pdftext.unmapped simple", pdftone(&a,
               "BT /F1 12 Tf 72 700 Td (Tab\tthen ) Tj /F2 12 Tf (\001\002) Tj ET", res),
               "Tab then \xef\xbf\xbd\n"
               "[2 characters on this page are in fonts with no Unicode mapping]\n");
    }

    // One never ended stands for nothing: the glyphs it covered are read
    expect(t, "pdftext.actualtext unclosed", pdftone(&a,
           "BT /F1 12 Tf 72 700 Td (Unclosed: ) Tj /Span << /ActualText (KEEP) >> BDC "
           "(xyz) Tj 0 -14 Td (Next line.) Tj ET"),
           "Unclosed: xyz\nNext line.\n");

    // A form opening a span it never closes, drawn many times: each
    // span's text once took a copy in page, which ran out of memory
    {
        Buf at(&a, 4096);
        print(&at, "<< /Font << /F1 << /Type /Font /Subtype /Type1 /BaseFont /Helvetica "
                   ">> >> /XObject << /Fm 3 0 R >> /Properties << /P0 << /ActualText (");
        for (i32 i = 0; i < 4000; i++) {
            putbyte(&at, (u8)('a' + i%26));
        }
        print(&at, ") >> >> >>");
        Str extra[] = {
            pdfbstream(&a, "/Type /XObject /Subtype /Form /BBox [0 0 600 800]",
                       "/Span /P0 BDC"),
        };
        Buf c(&a, 1<<14);
        for (i32 i = 0; i < 400; i++) {
            print(&c, "/Fm Do ");
        }
        print(&c, "BT /F1 12 Tf 72 700 Td (After the forms.) Tj ET");
        Str    content = finish(&c);
        Str    doc     = pdftdoc(&a, finish(&at), &content, 1, extra, countof(extra));
        Arena  s       = a;
        PdfOut r       = pdftread(&s, doc, {}, 1<<20);
        expect(t, "pdftext.actualtext form repeated",
               r.why.data ? r.why : r.text, "--- page 1 ---\nAfter the forms.\n");
    }

    // A form with its own resources and matrix, inside a form; a form
    // that draws itself stops at itself
    {
        Str extra[] = {
            pdfbstream(&a, "/Type /XObject /Subtype /Form /BBox [0 0 600 800] "
                       "/Matrix [1 0 0 1 0 -100] /Resources << /XObject << /In 4 0 R >> >>",
                       "/In Do"),
            pdfbstream(&a, "/Type /XObject /Subtype /Form /BBox [0 0 600 800] "
                       "/Resources << /Font << /F9 << /Type /Font /Subtype /Type1 "
                       "/BaseFont /Courier >> >> /XObject << /Me 4 0 R >> >>",
                       "BT /F9 10 Tf 72 700 Td (inner) Tj ET /Me Do"),
        };
        expect(t, "pdftext.form", pdftone(&a,
               "BT /F1 12 Tf 72 700 Td (outer) Tj ET /Out Do",
               "<< /Font << /F1 << /Type /Font /Subtype /Type1 /BaseFont /Helvetica >> >> "
               "/XObject << /Out 3 0 R >> >>", extra, countof(extra)),
               "outer\n\ninner\n");
    }

    // Images: an XObject drawn twice on a page and once on the next keeps
    // its number, placed by its top among the lines; a full-page one;
    // an inline image, its data skipped and its offset kept; JPX noted
    {
        Str img = pdfbstream(&a, "/Type /XObject /Subtype /Image /Width 48 /Height 9 "
                             "/ColorSpace /DeviceRGB /BitsPerComponent 8", "xyz");
        Str extra[] = {
            img,
            pdfbstream(&a, "/Type /XObject /Subtype /Image /Width 600 /Height 800 "
                       "/ColorSpace /DeviceGray /BitsPerComponent 8 /Filter /JPXDecode", "j"),
        };
        Str res = "<< /Font << /F1 << /Type /Font /Subtype /Type1 /BaseFont /Helvetica >> >> "
                  "/XObject << /Im 3 0 R /Jp 4 0 R >> >>";
        Str pages[] = {
            "BT /F1 12 Tf 72 700 Td (above) Tj ET q 100 0 0 50 72 600 cm /Im Do Q "
            "q 100 0 0 50 72 300 cm /Im Do Q BT /F1 12 Tf 72 500 Td (below) Tj ET "
            "BI /W 2 /H 2 /CS /G /BPC 8 ID \x01" "EI\x02\x03\n EI BT /F1 12 Tf 72 100 Td "
            "(after) Tj ET",
            "q 612 0 0 792 0 0 cm /Im Do Q q 600 0 0 800 0 0 cm /Jp Do Q",
            "",
        };
        PdfOut r = pdftread(&a, pdftdoc(&a, res, pages, countof(pages), extra, countof(extra)));
        expect(t, "pdftext.images", r.text,
               "--- page 1 ---\nabove\n\n[image 1: 48x9 RGB]\nbelow\n\nafter\n[image 2: 2x2 gray]\n"
               "--- page 2 ---\n[image 3: 600x800, the whole page, not shown (JPEG 2000)]\n"
               "[image 1: 48x9 RGB, the whole page]\n"
               "[no text on this page, only images]\n"
               "--- page 3 ---\n[no text on this page]\n");
        expect(t, "pdftext.places", pdfplaces(&a, r),
               "1 1 4 3 -1 -1 48x9 RGB;1 1 4 3 -1 -1 48x9 RGB;2 1 8 8 0 138 2x2 gray;"
               "1 2 11 3 -1 -1 48x9 RGB whole;3 2 10 4 -1 -1 600x800 JPEG 2000 whole;");
        Buf lines(&a, 64);
        for (iz i = 0; i < r.pageline.len; i++) {
            print(&lines, r.pageline.data[i]);
            putbyte(&lines, ' ');
        }
        expect(t, "pdftext.pageline", finish(&lines), "1 9 13 ");
    }

    // A page can't forge a marker: a line that would read as one gets a
    // leading space
    expect(t, "pdftext.forged", pdftone(&a,
           "BT /F1 12 Tf 72 700 Td (--- page 9 ---) Tj ET"), " --- page 9 ---\n");

    // Refusals leave perm as it was
    {
        i32  nb   = 0;
        Str *ob   = pdfbdoc(&a, &nb);
        Str  bad[] = {"<html>", "%PDF-1.4\n", pdfbuild(&a, ob, nb, PDFB_ENCRYPT)};
        Str  want[] = {"2 it is not a PDF", "4 it has no pages", "3 it is encrypted"};
        for (i32 i = 0; i < countof(bad); i++) {
            Arena perm = carve(&a, 1<<16);
            byte *beg  = perm.beg;
            PdfOut r   = pdfread(&perm, a, bad[i], 0);
            Str    got = concat(&a, concat(&a, istr(&a, r.status), " "), r.why);
            expect(t, "pdftext.refuse", got, want[i]);
            expect(t, "pdftext.refuse perm", istr(&a, perm.beg-beg), "0");
        }
    }

    // A file pdfsniff takes for a PDF in which the reader found none:
    // one only if it is binary or begins as one does (pdflikely)
    {
        Str bin = {(u8 *)"x %PDF-1.4\0", 11};
        expect(t, "pdflikely header", istr(&a, pdflikely("%PDF-1.4\nno pages")), "1");
        expect(t, "pdflikely binary", istr(&a, pdflikely(bin)), "1");
        expect(t, "pdflikely mention",
               istr(&a, pdflikely("Every PDF begins %PDF-1.7.\n")), "0");
    }

    // Last, as each starts over from t->perm
    testreadpdfmemo(t);
    testpdfattachnoimages(t);
}

// A document with a bit of everything, for the sweeps.
static Str pdftmixed(Arena *a)
{
    Str tou = "begincodespacerange <0000> <ffff> endcodespacerange "
              "1 beginbfrange <0003> <0010> <0041> endbfrange";
    Str extra[] = {
        "<< /Type /Font /Subtype /Type0 /BaseFont /X /Encoding /Identity-H "
            "/DescendantFonts [<< /Type /Font /Subtype /CIDFontType2 /W [3 16 600] >>] "
            "/ToUnicode 4 0 R >>",
        pdfbstream(a, "", tou),
        pdfbstream(a, "/Subtype /Image /Width 8 /Height 8 /ColorSpace /DeviceGray "
                   "/BitsPerComponent 8 /Filter /ASCIIHexDecode", "00ff>"),
        pdfbstream(a, "/Subtype /Form /BBox [0 0 612 792] /Matrix [1 0 0 1 0 -50]",
                   "BT /F1 10 Tf 72 300 Td (In a form) Tj ET q 50 0 0 50 300 300 cm /Im Do Q"),
    };
    Str res = "<< /Font << /F1 << /Type /Font /Subtype /Type1 /BaseFont /Times-Roman >> "
              "/F2 3 0 R >> /XObject << /Im 5 0 R /Fm 6 0 R >> >>";
    Str pages[] = {
        "BT /F1 12 Tf 72 700 Td (The first page, a line of words.) Tj 0 -14 TD "
            "[(Kerned) -300 (words)] TJ T* (more) ' ET /Fm Do",
        "BT /F2 12 Tf 72 700 Td <000300040005> Tj ET q 100 0 0 100 72 500 cm /Im Do Q "
            "BI /W 1 /H 1 /CS /G /BPC 8 ID x EI BT /F1 9 Tf 72 400 Td (after) Tj ET",
        "",
        "BT /F1 12 Tf 72 700 Td (--- page 1 ---) Tj 0 -30 Td (The end.) Tj ET",
    };
    return pdftdoc(a, res, pages, countof(pages), extra, countof(extra));
}

static void test_pdftextsweep(Test *t)
{
    Arena  a    = t->perm;
    Str    pdf  = pdftmixed(&a);
    PdfOut full = pdftread(&a, pdf);
    expect(t, "pdftext.mixed", full.text,
           "--- page 1 ---\nThe first page, a line of words.\nKerned words\nmore\n"
           "\nIn a form\n[image 1: 8x8 gray]\n"
           "--- page 2 ---\nABC\n\n[image 1: 8x8 gray]\nafter\n[image 2: 1x1 gray]\n"
           "--- page 3 ---\n[no text on this page]\n"
           "--- page 4 ---\n --- page 1 ---\n\nThe end.\n");
    expect(t, "pdftext.mixed valid", pdfoutbad(&full, &a), {});

    // Too little work: what is read is valid, and its whole pages are
    // the full read's
    {
        b32 ok   = 1;
        i32 cut  = 0;
        for (i64 w = 1; ok && w <= 1<<16; w = w*9/8 + 1) {
            Arena  s = a;
            PdfOut r = pdftread(&s, pdf, {.work=w});
            Str    bad = pdfoutbad(&r, &s);
            Str    pre = pdfwholepages(r);
            ok &= !bad.data;
            ok &= r.status==PDFR_NOPAGES ||
                  (r.status==PDFR_OK && r.text==full.text) ||
                  (r.why=="it needs more work than is allowed" &&
                   takehead(full.text, pre.len)==pre);
            cut += r.status==PDFR_PARTIAL;
            if (!ok) {
                expect(t, "pdftext.worksweep", concat(&s, istr(&s, w), r.text), bad);
            }
        }
        expect(t, "pdftext.worksweep", istr(&a, ok && cut>=8), "1");
    }

    // Too little memory: each read is the full one, or cut short for
    // want of memory, valid either way. Finely over a page of many
    // glyphs, whose layout once lacked the room and called it empty.
    {
        Buf many(&a, 1<<15);
        print(&many, "BT /F1 9 Tf 40 760 Td 11 TL ");
        for (i32 i = 0; i < 60; i++) {
            print(&many, "(line ");
            print(&many, (i64)i);
            print(&many, " of the many words drawn on this page) Tj T* ");
        }
        print(&many, "ET");
        Str    mc    = finish(&many);
        Str    big   = pdftdoc(&a, PDFT_HELV, &mc, 1);
        PdfOut bfull = pdftread(&a, big);  // in a, which the reads below reuse
        Str    docs[]  = {pdf, big};
        Str    fulls[] = {full.text, bfull.text};
        b32 ok   = bfull.status == PDFR_OK;
        b32 some = 0;
        for (i32 d = 0; d < 2; d++) {
            for (iz room = 1<<12; ok && room <= 1<<25; room += room/16 + 1) {
                Arena  s = a;
                PdfOut r = pdftread(&s, docs[d], {}, room);
                Str    pre = pdfwholepages(r);
                ok &= !pdfoutbad(&r, &s).data;
                ok &= (r.status==PDFR_OK && r.text==fulls[d]) ||
                      ((r.why=="out of memory") &&
                       takehead(fulls[d], pre.len)==pre);
                some |= r.status == PDFR_OK;
                if (!ok) {
                    expect(t, "pdftext.memsweep", concat(&s, istr(&s, room), r.text), r.why);
                }
            }
        }
        expect(t, "pdftext.memsweep", istr(&a, ok && some), "1");
    }

    // Layout never gives up on a step for memory and carries on without
    // it: lines on two baselines a little apart join into one, which is
    // cut back into two only with room for the lines again. Without it
    // the letters once came out interleaved, the read called complete.
    {
        Buf tg(&a, 1<<15);
        print(&tg, "BT /F1 12 Tf 72 760 Td ");
        for (i32 i = 0; i < 24; i++) {
            print(&tg, "(Inventory note ");
            print(&tg, (i64)i);
            print(&tg, ": nothing unusual) Tj 0 -5.5 Td (Inventory note ");
            print(&tg, (i64)i+1);
            print(&tg, ": nothing unusual) Tj 0 -24.5 Td ");
        }
        print(&tg, "ET");
        Str    tc   = finish(&tg);
        Str    doc  = pdftdoc(&a, PDFT_HELV, &tc, 1);
        PdfOut tful = pdftread(&a, doc);  // its text in a, not overwritten
        expect(t, "pdftext.tangled",
               istr(&a, startswith(tful.text, "--- page 1 ---\nInventory note 0: nothing unusual\n"
                                              "Inventory note 1: nothing unusual\n")), "1");
        b32 ok   = tful.status == PDFR_OK;
        i32 oom  = 0;
        i32 same = 0;
        for (iz room = 1<<16; ok && room <= 1<<23; room += room/256 + 1) {
            Arena  s = a;
            PdfOut r = pdftread(&s, doc, {}, room);
            ok &= !pdfoutbad(&r, &s).data;
            ok &= (r.status==PDFR_OK && r.text==tful.text) || r.why=="out of memory";
            oom  += r.why == "out of memory";
            same += r.status == PDFR_OK;
            if (!ok) {
                expect(t, "pdftext.tangledsweep", concat(&s, istr(&s, room), r.text), r.why);
            }
        }
        expect(t, "pdftext.tangledsweep", istr(&a, ok && oom && same), "1");
    }

    // Cut anywhere and corrupted anywhere, read without fault, valid
    {
        i32 bad = 0;
        for (iz n = 0; n <= pdf.len; n += 7) {
            Arena s   = a;
            u8   *cpy = allocbytes(&s, n);
            copybytes(cpy, pdf.data, n);
            PdfOut r = pdftread(&s, {cpy, n});
            bad += !!pdfoutbad(&r, &s).data;
        }
        for (iz i = 0; i < pdf.len; i += 3) {
            Arena s   = a;
            u8   *cpy = allocbytes(&s, pdf.len);
            copybytes(cpy, pdf.data, pdf.len);
            cpy[i] ^= (u8)(1 + i%255);
            PdfOut r = pdftread(&s, {cpy, pdf.len});
            bad += !!pdfoutbad(&r, &s).data;
        }
        expect(t, "pdftext.cut corrupt", istr(&a, bad), "0");
    }

    // Two reads at different offsets agree
    {
        Arena s = a;
        allocbytes(&s, 13);
        PdfOut r = pdftread(&s, pdf);
        expect(t, "pdftext.offset", r.text, full.text);
    }
}

// A page's text, as pdftone, the page given extra entries (/Annots).
static Str pdftpage(Arena *a, Str content, Str res, Str *extra, i32 ne, Str pageextra)
{
    PdfOut r = pdftread(a, pdftdoc(a, res, &content, 1, extra, ne, pageextra));
    Str    s = r.text;
    if (startswith(s, "--- page 1 ---\n")) {
        s = cuthead(s, 15);
    }
    return r.why.data ? concat(a, concat(a, s, "! "), r.why) : s;
}

// Lines of text at x, from y down by step, one BT each.
static Str pdftlines(Arena *a, f64 x, f64 y, f64 step, Str *lines, i32 n, Str font = "/F1 12 Tf")
{
    Buf b(a, 1024);
    for (i32 i = 0; i < n; i++) {
        print(&b, "BT ");
        print(&b, font);
        putbyte(&b, ' ');
        pdfprintreal(&b, x);
        putbyte(&b, ' ');
        pdfprintreal(&b, y - step*i);
        print(&b, " Td (");
        print(&b, lines[i]);
        print(&b, ") Tj ET\n");
    }
    return finish(&b);
}

// Work a file can repeat without bound is charged: an /ActualText named
// by /Properties, copied and walked at each BDC; resource lookups,
// scans of every key, at each Tf, Do and BDC; annotations' lookups.
static Str pdftbudget(Arena *a, Str res, Str content, Str *extra, i32 ne)
{
    // Deflated, so that the file's size, and the work it buys, is small
    Str objs[5] = {};
    objs[0] = "<< /Type /Catalog /Pages 2 0 R >>";
    objs[1] = concat(a, concat(a, "<< /Type /Pages /MediaBox [0 0 612 792] "
                     "/Kids [3 0 R] /Count 1 /Resources ", res), " >>");
    objs[2] = "<< /Type /Page /Parent 2 0 R /Contents 4 0 R >>";
    objs[3] = pdfbstream(a, "/Filter /FlateDecode", pdfbzlib(a, content));
    objs[4] = ne ? extra[0] : Str("null");
    PdfOut r = pdftread(a, pdfbuild(a, objs, 5, 0));
    return r.why.data ? r.why : Str("ok");
}

static void test_pdfbudget(Test *t)
{
    Arena a = t->perm;
    {
        Buf at(&a, 1<<15);
        print(&at, "<< /ActualText (");
        for (i32 i = 0; i < 20000; i++) putbyte(&at, 'A');
        print(&at, ") >>");
        Str extra = finish(&at);
        Buf c(&a, 1<<13);
        for (i32 i = 0; i < 400; i++) print(&c, "/P0 BDC EMC\n");
        expect(t, "pdfbudget actualtext",
               pdftbudget(&a, "<< /Properties << /P0 5 0 R >> >>", finish(&c), &extra, 1),
               "it needs more work than is allowed");
    }
    for (i32 k = 0; k < 3; k++) {
        Str  kinds[] = {"XObject", "Font", "Properties"};
        Str  ops[]   = {"/Nope Do\n", "/Nope 12 Tf\n", "/Nope BDC EMC\n"};
        Buf  res(&a, 1<<15);
        print(&res, "<< /");
        print(&res, kinds[k]);
        print(&res, " << ");
        for (i32 i = 0; i < 2000; i++) {
            print(&res, "/k");
            print(&res, (i64)i);
            print(&res, " 0 ");
        }
        print(&res, ">> >>");
        Buf c(&a, 1<<20);
        for (i32 i = 0; i < 40000; i++) print(&c, ops[k]);
        expect(t, concat(&a, "pdfbudget lookup ", kinds[k]),
               pdftbudget(&a, finish(&res), finish(&c), 0, 0),
               "it needs more work than is allowed");
    }
    // Marked content a form leaves open ends with the form: a stray EMC
    // after it does not end the form's ActualText
    {
        Str form = pdfbstream(&a, "/Type /XObject /Subtype /Form /BBox [0 0 612 792]",
                              "/Span << /ActualText (X) >> BDC");
        Str res  = "<< /Font << /F1 << /Type /Font /Subtype /Type1 /BaseFont /Helvetica >> >> "
                   "/XObject << /Fm 3 0 R >> >>";
        expect(t, "pdfbudget form bdc",
               pdftone(&a, "q /Fm Do Q BT /F1 12 Tf 72 700 Td (Hello world) Tj ET EMC",
                       res, &form, 1),
               "Hello world\n");
    }
}

static void test_pdflayout(Test *t)
{
    Arena a = t->perm;

    // Two columns drawn line by line across both read column by column
    {
        Str l[] = {"The left column is about", "the weather on the coast,", "which was mild",
                   "through most of", "the season."};
        Str r[] = {"The right column holds", "the vault number, 4616,", "and the guard,",
                   "name, Fennel.", "Nothing else."};
        Buf b(&a, 2048);
        for (i32 i = 0; i < countof(l); i++) {
            print(&b, pdftlines(&a, 72, 700-16*i, 0, l+i, 1));
            print(&b, pdftlines(&a, 330, 700-16*i, 0, r+i, 1));
        }
        expect(t, "pdflayout.columns", pdftone(&a, finish(&b)),
               "The left column is about\nthe weather on the coast,\nwhich was mild\n"
               "through most of\nthe season.\n\nThe right column holds\n"
               "the vault number, 4616,\nand the guard,\nname, Fennel.\nNothing else.\n");
    }

    // A heading in the right column between two of the left's baselines
    // joins neither, and the two stay apart, not letter between letter
    {
        Str l[] = {"Left one of the column", "Left two of the column",
                   "Left three of the column", "Left four of the column",
                   "Left five of the column", "Left six of the column"};
        Str r[] = {"Right one goes here", "Right two goes here", "Right three goes here"};
        Str h[] = {"2 Methods"};
        Str m[] = {"Later one goes here", "Later two goes here"};
        Str s = concat(&a, concat(&a, concat(&a,
                pdftlines(&a, 72, 700, 12, l, 6, "/F1 10 Tf"),
                pdftlines(&a, 320, 700, 12, r, 3, "/F1 10 Tf")),
                pdftlines(&a, 320, 657, 0, h, 1, "/F1 14 Tf")),
                pdftlines(&a, 320, 640, 12, m, 2, "/F1 10 Tf"));
        expect(t, "pdflayout.columns heading between", pdftone(&a, s),
               "Left one of the column\nLeft two of the column\nLeft three of the column\n"
               "Left four of the column\nLeft five of the column\nLeft six of the column\n\n"
               "Right one goes here\nRight two goes here\nRight three goes here\n"
               "2 Methods\nLater one goes here\nLater two goes here\n");
    }

    // A gutter as narrow as LaTeX's (10 points at 10 point type) holds
    // past a larger heading in one column
    {
        Str res = "<< /Font << /F1 << /Type /Font /Subtype /Type1 /BaseFont /Courier >> >> >>";
        Str l[] = {"Left one, set in a fixed width font:..", "Left two, set in a fixed width font:..",
                   "Left three, set in a fixed width font.", "Left four, set in a fixed width font:.",
                   "Left five, set in a fixed width font:.", "Left six, set in a fixed width font:..",
                   "Left seven, set in a fixed width font."};
        Str r[] = {"Right one sits here", "Right two sits here", "Right three sits here"};
        Str h[] = {"2 Methods"};
        Str m[] = {"Later one sits here", "Later two sits here", "Later three sits here"};
        Str s = concat(&a, concat(&a, concat(&a,
                pdftlines(&a, 72, 700, 12, l, 7, "/F1 10 Tf"),
                pdftlines(&a, 310, 700, 12, r, 3, "/F1 10 Tf")),
                pdftlines(&a, 310, 660, 0, h, 1, "/F1 16 Tf")),
                pdftlines(&a, 310, 640, 12, m, 3, "/F1 10 Tf"));
        Buf want(&a, 1024);
        for (i32 i = 0; i < countof(l); i++) {
            print(&want, l[i]);
            putbyte(&want, '\n');
        }
        print(&want, "\nRight one sits here\nRight two sits here\nRight three sits here\n"
                     "2 Methods\nLater one sits here\nLater two sits here\nLater three sits here\n");
        expect(t, "pdflayout.columns narrow gutter", pdftone(&a, s, res), finish(&want));
    }

    // A table of short cells is not columns: it reads in rows, two spaces
    // between cells even where their gap is under a wide one
    {
        Str c0[] = {"Shelf", "A", "B", "C"};
        Str c1[] = {"Boxes", "12", "1768", "7"};
        Str c2[] = {"Keeper", "Morrow", "Magpie", "Hale"};
        Str s = concat(&a, concat(&a, pdftlines(&a, 72, 700, 14, c0, 4),
                                      pdftlines(&a, 112, 700, 14, c1, 4)),
                                      pdftlines(&a, 158, 700, 14, c2, 4));
        expect(t, "pdflayout.table", pdftone(&a, s),
               "Shelf  Boxes  Keeper\nA  12  Morrow\nB  1768  Magpie\nC  7  Hale\n");
    }

    // A table of wordy cells reads as columns, but lines below it that
    // reach across the gutter the table test dropped are not its first
    // column's: they read after it
    {
        Str c0[] = {"Name", "Alpha unit", "Beta unit", "Gamma unit"};
        Str c1[] = {"Description of the item", "A small red box of nails",
                    "A large blue crate of tools", "The green bag of spare parts"};
        Str c2[] = {"Notes for the reader", "Keep it away from water",
                    "Ships only on Mondays", "Code word is quince here"};
        Str k[]  = {"Invoice number:", "Customer:", "Due date:"};
        Str v[]  = {"INV 2024 0042", "Harbour Supplies Ltd", "12 March 2025"};
        Str s = concat(&a, concat(&a, concat(&a, concat(&a,
                pdftlines(&a, 72, 700, 14, c0, 4, "/F1 10 Tf"),
                pdftlines(&a, 160, 700, 14, c1, 4, "/F1 10 Tf")),
                pdftlines(&a, 330, 700, 14, c2, 4, "/F1 10 Tf")),
                pdftlines(&a, 72, 600, 14, k, 3, "/F1 10 Tf")),
                pdftlines(&a, 200, 600, 14, v, 3, "/F1 10 Tf"));
        expect(t, "pdflayout.table wordy", pdftone(&a, s),
               "Name  Description of the item\nAlpha unit  A small red box of nails\n"
               "Beta unit  A large blue crate of tools\n"
               "Gamma unit  The green bag of spare parts\n\n"
               "Notes for the reader\nKeep it away from water\nShips only on Mondays\n"
               "Code word is quince here\n\n"
               "Invoice number:  INV 2024 0042\nCustomer:  Harbour Supplies Ltd\n"
               "Due date:  12 March 2025\n");
    }

    // A table in the second column: its own gutter is dropped first, as
    // short cells are on both sides, and the columns stay
    {
        Str l[] = {"Reservoirs fill from the moor", "and the town measures them",
                   "every week, with the dams", "walked at dawn by keepers."};
        Str n[] = {"Upper Moor", "Thistle", "Low Field"};
        Str v[] = {"2210", "9914", "1875"};
        Str s = concat(&a, concat(&a, pdftlines(&a, 72, 700, 14, l, 4),
                                      pdftlines(&a, 330, 700, 14, n, 3)),
                                      pdftlines(&a, 430, 700, 14, v, 3));
        expect(t, "pdflayout.tablecol", pdftone(&a, s),
               "Reservoirs fill from the moor\nand the town measures them\n"
               "every week, with the dams\nwalked at dawn by keepers.\n\n"
               "Upper Moor  2210\nThistle  9914\nLow Field  1875\n");
    }

    // A heading over three columns that reaches into the first gutter is
    // a band of its own, so the gutter stays
    {
        Str h[]  = {"Harbour archive"};
        Str c0[] = {"alpha beta gamma", "delta epsilon zeta", "eta theta iota"};
        Str c1[] = {"one two three", "four five six", "seven eight nine"};
        Str c2[] = {"red green blue", "cyan magenta yellow", "black white grey"};
        Str s = concat(&a, concat(&a, concat(&a,
                pdftlines(&a, 72, 720, 0, h, 1, "/F1 24 Tf"),
                pdftlines(&a, 72, 690, 14, c0, 3)),
                pdftlines(&a, 248, 690, 14, c1, 3)),
                pdftlines(&a, 420, 690, 14, c2, 3));
        expect(t, "pdflayout.heading", pdftone(&a, s),
               "Harbour archive\n\nalpha beta gamma\ndelta epsilon zeta\neta theta iota\n\n"
               "one two three\nfour five six\nseven eight nine\n\n"
               "red green blue\ncyan magenta yellow\nblack white grey\n");
    }

    // Lines a step below three columns that narrowed a gutter while the
    // band ran on are cut off, and the gutter comes back (once lost, so
    // that two columns read as rows)
    {
        Str c0[] = {"alpha beta gamma", "delta epsilon zeta", "eta theta iota"};
        Str c1[] = {"one two three", "four five six", "seven eight nine"};
        Str c2[] = {"red green blue", "cyan magenta yellow", "black white grey"};
        Str x[]  = {"This line crosses the first gutter, wide"};
        Str s = concat(&a, concat(&a, concat(&a,
                pdftlines(&a, 72, 690, 14, c0, 3),
                pdftlines(&a, 248, 690, 14, c1, 3)),
                pdftlines(&a, 420, 690, 14, c2, 3)),
                pdftlines(&a, 72, 622, 14, x, 1));
        expect(t, "pdflayout.retrim", pdftone(&a, s),
               "alpha beta gamma\ndelta epsilon zeta\neta theta iota\n\n"
               "one two three\nfour five six\nseven eight nine\n\n"
               "red green blue\ncyan magenta yellow\nblack white grey\n\n"
               "This line crosses the first gutter, wide\n");
    }

    // A first column longer than the second keeps its end; a heading a
    // step below the columns reads after them (once read before the
    // second column, and the first column's end after it)
    {
        Str l[] = {"The first column runs on", "for longer than the other,",
                   "line after line of it,", "and still more of it", "until it ends here."};
        Str r[] = {"The second column is", "shorter than the first", "and ends early."};
        Str x[] = {"Next", "This line runs across both columns of the page, wide."};
        Str s = concat(&a, concat(&a, pdftlines(&a, 72, 700, 14, l, 5),
                                      pdftlines(&a, 330, 700, 14, r, 3)),
                                      pdftlines(&a, 72, 604, 14, x, 2));
        expect(t, "pdflayout.tail", pdftone(&a, s),
               "The first column runs on\nfor longer than the other,\nline after line of it,\n"
               "and still more of it\nuntil it ends here.\n\nThe second column is\n"
               "shorter than the first\nand ends early.\n\nNext\n"
               "This line runs across both columns of the page, wide.\n");
    }

    // Letter spacing (0.4 em) is not read as spaces; the word gap still is
    expect(t, "pdflayout.tracking", pdftone(&a,
           "BT /F1 12 Tf 4.8 Tc 72 700 Td [(ember) -1000 (fjord)] TJ ET"),
           "ember fjord\n");

    // Fake bold: a line drawn again half a point away reads once, its
    // doubled letters kept
    expect(t, "pdflayout.fakebold", pdftone(&a,
           "BT /F1 12 Tf 72 700 Td (Still bold: cactus 1984.) Tj ET "
           "BT /F1 12 Tf 72.5 700.3 Td (Still bold: cactus 1984.) Tj ET"),
           "Still bold: cactus 1984.\n");

    // Right to left: Hebrew drawn in visual order reads in logical order,
    // its number's digits in theirs and brackets mirrored; on a line with
    // left-to-right text at an end only the Hebrew run turns
    {
        Str res = "<< /Font << /F1 << /Type /Font /Subtype /Type1 /BaseFont /Helvetica >> "
                  "/F2 << /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding "
                  "<< /Differences [65 /uni05D0 /uni05D5 /uni05D4 /uni05D3 /uni05E7] >> >> >> >>";
        expect(t, "pdflayout.rtl", pdftone(&a,
               "BT /F2 12 Tf 72 700 Td (\\(6831\\) ABC DBEC) Tj 0 -14 Td "
               "(Hebrew: 6831 ABC DBEC) Tj 0 -14 Td (12.5 ABC) Tj ET", res),
               "\xd7\x94\xd7\xa7\xd7\x95\xd7\x93 \xd7\x94\xd7\x95\xd7\x90 (6831)\n"
               "Hebrew: 6831 \xd7\x94\xd7\xa7\xd7\x95\xd7\x93 \xd7\x94\xd7\x95\xd7\x90\n"
               "\xd7\x94\xd7\x95\xd7\x90 12.5\n");
    }

    {
        Str res = "<< /Font << /F1 << /Type /Font /Subtype /Type1 /BaseFont /Helvetica >> "
                  "/F2 << /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding "
                  "<< /Differences [65 /uni05D0 /uni05D5 /uni05D4 /uni05D3 /uni05E7] >> >> >> >>";
        // Right to left at both ends: a Latin word inside keeps its order
        expect(t, "pdflayout.rtl latin inside", pdftone(&a,
               "BT /F2 12 Tf 72 700 Td (AB xyz DE) Tj ET", res),
               "\xd7\xa7\xd7\x93 xyz \xd7\x95\xd7\x90\n");
        expect(t, "pdflayout.rtl latin phrase", pdftone(&a,
               "BT /F2 12 Tf 72 700 Td (ABCD report.pdf 2024 DEDE ABAB) Tj ET", res),
               "\xd7\x95\xd7\x90\xd7\x95\xd7\x90 \xd7\xa7\xd7\x93\xd7\xa7\xd7\x93 "
               "report.pdf 2024 \xd7\x93\xd7\x94\xd7\x95\xd7\x90\n");
        // Left to right at the ends: each Hebrew run turns alone, in place
        expect(t, "pdflayout.rtl two runs", pdftone(&a,
               "BT /F2 12 Tf 72 700 Td (The words AB and DE are greetings.) Tj ET", res),
               "The words \xd7\x95\xd7\x90 and \xd7\xa7\xd7\x93 are greetings.\n");
    }
    expect(t, "pdftext.cmap many overrides", pdfcmapdump(&a,
           "1 begincodespacerange <0000> <ffff> endcodespacerange "
           "1 beginbfrange <0000> <00ff> <0041> endbfrange "
           "9 beginbfchar <0001> <0062> <0002> <0062> <0003> <0062> <0004> <0062> <0005> <0062> "
           "<0006> <0062> <0007> <0062> <0008> <0062> <0009> <0062> endbfchar",
           "\x00\x50"), "0050/2=0091 r11");
    // Overlaps are cut apart: a range inside a wider one, the wider going
    // on past it, its codes still counted from its own start
    expect(t, "pdftext.cmap nested", pdfcmapdump(&a,
           "1 begincodespacerange <0000> <ffff> endcodespacerange "
           "2 beginbfrange <0000> <00c8> <0041> <0032> <0064> <0061> endbfrange",
           Str((u8 *)"\x00\x14\x00\x3c\x00\x96\x00\xc9", 8)),
           "0014/2=0055 003c/2=006b 0096/2=00d7 00c9/2#0 r3");
    expect(t, "pdftext.cmap later wins", pdfcmapdump(&a,
           "1 begincodespacerange <00> <ff> endcodespacerange "
           "2 beginbfchar <41> <0061> <41> <0062> endbfchar", "A"), "41/1=0062 r1");
    {
        Str tou = "1 begincodespacerange <0000> <ffff> endcodespacerange "
                  "1 beginbfchar <0041> <005A> endbfchar";
        Str extra[] = {
            "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /ToUnicode 4 0 R >>",
            pdfbstream(&a, "", tou),
        };
        expect(t, "pdftext.tounicode padded", pdftone(&a, "BT /F1 10 Tf 72 700 Td (AB) Tj ET",
               "<< /Font << /F1 3 0 R >> >>", extra, countof(extra)), "ZB\n");
    }
    {
        Str tou = "begincodespacerange <0000> <ffff> endcodespacerange "
                  "2 beginbfchar <0020> <0078> <0041> <0079> endbfchar";
        Str extra[] = {
            "<< /Type /Font /Subtype /Type0 /BaseFont /X /Encoding /Identity-H "
                "/DescendantFonts [4 0 R] /ToUnicode 5 0 R >>",
            "<< /Type /Font /Subtype /CIDFontType2 /BaseFont /X /DW 500 >>",
            pdfbstream(&a, "", tou),
        };
        expect(t, "pdftext.type0 no word spacing", pdftone(&a,
               "BT /F1 10 Tf 30 Tw 72 700 Td <00200041> Tj ET",
               "<< /Font << /F1 3 0 R >> >>", extra, countof(extra)), "xy\n");
    }
    {
        Str res = "<< /Font << /F1 << /Type /Font /Subtype /Type1 /BaseFont /Helvetica >> "
                  "/F2 << /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding "
                  "<< /Differences [65 /uni0301] >> >> >> >>";
        expect(t, "pdflayout.fakebold marks", pdftone(&a,
               "BT /F1 12 Tf 72 700 Td (cafe) Tj ET BT /F2 12 Tf 87.5 700 Td (A) Tj ET "
               "BT /F1 12 Tf 72.5 700 Td (cafe) Tj ET BT /F2 12 Tf 88 700 Td (A) Tj ET", res),
               "cafe\xcc\x81\n");
    }
    expect(t, "pdflayout.tracking by TJ", pdftone(&a,
           "BT /F1 12 Tf 72 700 Td [(e) -400 (m) -400 (b) -400 (e) -400 (r) -1400 (f) -400 "
           "(j) -400 (o) -400 (r) -400 (d)] TJ ET"), "ember fjord\n");
    {
        Str tou = "begincodespacerange <0000> <ffff> endcodespacerange "
                  "3 beginbfchar <0001> <4E00> <0002> <4E8C> <0003> <4E09> endbfchar";
        Str extra[] = {
            "<< /Type /Font /Subtype /Type0 /BaseFont /X /Encoding /Identity-H "
                "/DescendantFonts [4 0 R] /ToUnicode 5 0 R >>",
            "<< /Type /Font /Subtype /CIDFontType2 /BaseFont /X /DW 1000 >>",
            pdfbstream(&a, "", tou),
        };
        expect(t, "pdflayout.cjk gaps", pdftone(&a,
               "BT /F1 10 Tf 72 700 Td [<0001> -300 <0002> -300 <0003>] TJ ET",
               "<< /Font << /F1 3 0 R >> >>", extra, countof(extra)),
               "\xe4\xb8\x80\xe4\xba\x8c\xe4\xb8\x89\n");
    }
    expect(t, "pdflayout.group tie", pdftone(&a,
           "BT /F1 12 Tf 72 700 Td (abc) Tj ET BT /F1 12 Tf 0 1 -1 0 300 300 Tm (xyz) Tj ET"),
           "abc\n\nxyz\n");
    expect(t, "pdflayout.line v from largest", pdftone(&a,
           "BT /F1 12 Tf 72 700 Td (Alpha) Tj ET BT /F1 7 Tf 72 693 Td (1) Tj ET "
           "BT /F1 12 Tf 76 686 Td (Beta) Tj ET BT /F1 12 Tf 72 672 Td (Gamma) Tj ET "
           "BT /F1 12 Tf 72 658 Td (Delta) Tj ET"),
           "Alpha\n1Beta\nGamma\nDelta\n");
    {
        Str res = "<< /Font << /F1 << /Type /Font /Subtype /Type1 /BaseFont /Helvetica >> "
                  "/F2 << /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding "
                  "<< /Differences [65 /uni05D0 /uni05D5 /uni05D4 /uni05D3 /uni05E7] >> >> >> >>";
        expect(t, "pdflayout.rtl gap break", pdftone(&a,
               "BT /F2 12 Tf 72 700 Td [(AB) -600 (DE)] TJ ET", res),
               "\xd7\xa7\xd7\x93 \xd7\x95\xd7\x90\n");
        Str l[] = {"AB AB AB AB", "AB AB AB AB", "AB AB AB AB"};
        Str r[] = {"DE DE DE DE", "DE DE DE DE", "DE DE DE DE"};
        Str s = concat(&a, pdftlines(&a, 72, 700, 14, l, 3, "/F2 12 Tf"),
                           pdftlines(&a, 330, 700, 14, r, 3, "/F2 12 Tf"));
        Str rl = "\xd7\xa7\xd7\x93 \xd7\xa7\xd7\x93 \xd7\xa7\xd7\x93 \xd7\xa7\xd7\x93\n";
        Str ll = "\xd7\x95\xd7\x90 \xd7\x95\xd7\x90 \xd7\x95\xd7\x90 \xd7\x95\xd7\x90\n";
        Str want = concat(&a, concat(&a, concat(&a, concat(&a, concat(&a, concat(&a, concat(&a,
                   rl, rl), rl), "\n"), ll), ll), ll), "");
        expect(t, "pdflayout.rtl columns", pdftone(&a, s, res), want);
    }
    // A number for a value, a choice's option text for its value, and a
    // hidden widget left out
    {
        Str extra[] = {
            "<< /Subtype /Widget /FT /Tx /T (age) /V 42 /Rect [200 595 300 615] >>",
            "<< /Subtype /Widget /FT /Ch /T (land) /V (us) /Opt [(xx) [(ca) (Canada)] "
                "[(us) (United States)]] /Rect [200 555 300 575] >>",
            "<< /Subtype /Widget /FT /Tx /T (secret) /V (hidden) /F 2 "
                "/Rect [200 515 300 535] >>",
        };
        expect(t, "pdflayout.field values", pdftpage(&a,
               "BT /F1 12 Tf 72 600 Td (Age:) Tj 0 -40 Td (Land:) Tj 0 -40 Td (End.) Tj ET",
               PDFT_HELV, extra, countof(extra), "/Annots [3 0 R 4 0 R 5 0 R]"),
               "Age:  [field age: 42]\nLand:  [field land: United States]\nEnd.\n");
    }
    {
        Str extra[] = {
            "<< /Subtype /Widget /FT /Tx /T (n) /V (v) /Rect [107.34 595 300 615] >>",
        };
        expect(t, "pdflayout.field abutting", pdftpage(&a,
               "BT /F1 12 Tf 72 600 Td (Name:) Tj ET", PDFT_HELV, extra, countof(extra),
               "/Annots [3 0 R]"), "Name: [field n: v]\n");
    }
    {
        Str l[] = {"one", "two", "three", "four", "five"};
        Str m[] = {"six"};
        expect(t, "pdflayout.para floor", pdftone(&a,
               concat(&a, pdftlines(&a, 72, 700, 10, l, 5), pdftlines(&a, 72, 645.5, 10, m, 1))),
               "one\ntwo\nthree\nfour\nfive\nsix\n");
    }
    {
        Str extra[] = {
            "<< /FT /Btn /Ff 49152 /T (r) /V /B /Kids [4 0 R 5 0 R] >>",
            "<< /Subtype /Widget /Parent 3 0 R /AS /Off /Rect [200 595 220 615] >>",
            "<< /Subtype /Widget /Parent 3 0 R /AS /B /Rect [200 555 220 575] >>",
        };
        expect(t, "pdflayout.radio kids", pdftpage(&a,
               "BT /F1 12 Tf 72 600 Td (First:) Tj 0 -40 Td (Second:) Tj ET",
               PDFT_HELV, extra, countof(extra), "/Annots [4 0 R 5 0 R]"),
               "First:\n\nSecond:  [field r: B]\n");
    }
    {
        Str extra[] = {
            "<< /Type /Outlines /First 4 0 R >>",
            "<< /Title (ByIndex) /Next 5 0 R /Dest [1 /Fit] >>",
            "<< /Title (ByName) /Dest (zeta) >>",
            "<< /Kids [7 0 R 8 0 R] >>",
            "<< /Limits [(a) (c)] /Names [(alpha) [9 0 R /Fit]] >>",
            "<< /Limits [(x) (zeta)] /Names [(zeta) << /D [10 0 R /Fit] >>] >>",
        };
        Str pages[] = {"BT /F1 12 Tf 72 700 Td (One.) Tj ET", "BT /F1 12 Tf 72 700 Td (Two.) Tj ET"};
        PdfOut r = pdftread(&a, pdftdoc(&a, PDFT_HELV, pages, 2, extra, countof(extra), {},
                                        "/Outlines 3 0 R /Names << /Dests 6 0 R >>"));
        expect(t, "pdflayout.outline dests", r.text,
               "[outline: 2 entries]\n  ByIndex (page 2)\n  ByName (page 2)\n"
               "--- page 1 ---\nOne.\n--- page 2 ---\nTwo.\n");
    }
    {
        Str img = pdfbstream(&a, "/Type /XObject /Subtype /Image /Width 48 /Height 48 "
                             "/ColorSpace /DeviceGray /BitsPerComponent 8", "xyz");
        Str res = "<< /Font << /F1 << /Type /Font /Subtype /Type1 /BaseFont /Helvetica >> >> "
                  "/XObject << /Im 3 0 R >> >>";
        expect(t, "pdflayout.image with upward text", pdftone(&a,
               "q 300 0 0 300 50 400 cm /Im Do Q "
               "BT /F1 12 Tf 0 1 -1 0 100 100 Tm (Written upward one.) Tj ET "
               "BT /F1 12 Tf 0 1 -1 0 114 100 Tm (Written upward two.) Tj ET", res, &img, 1),
               "Written upward one.\nWritten upward two.\n[image 1: 48x48 gray]\n");
    }
    // Rotated text is a group of its own, after the main one, read in its
    // frame; the page's /Rotate changes nothing
    {
        Str s = "BT /F1 12 Tf 72 700 Td (Across the page.) Tj 0 -14 Td (Second line.) Tj ET "
                "BT /F1 12 Tf 0 1 -1 0 300 300 Tm (Written upward, 4421.) Tj ET";
        Str want = "Across the page.\nSecond line.\n\nWritten upward, 4421.\n";
        expect(t, "pdflayout.rotated", pdftone(&a, s), want);
        expect(t, "pdflayout.rotate90", pdftpage(&a, s, PDFT_HELV, 0, 0, "/Rotate 90"), want);
    }

    // A combining mark drawn after its base, a little left of it, follows
    // it, and the word after is still apart
    {
        Str res = "<< /Font << /F1 << /Type /Font /Subtype /Type1 /BaseFont /Helvetica >> "
                  "/F2 << /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding "
                  "<< /Differences [65 /uni0301] >> >> >> >>";
        expect(t, "pdflayout.marks", pdftone(&a,
               "BT /F1 12 Tf 72 700 Td (cafe) Tj ET BT /F2 12 Tf 87.5 700 Td (A) Tj ET "
               "BT /F1 12 Tf 98 700 Td (noir) Tj ET", res),
               "cafe\xcc\x81 noir\n");
    }

    // Paragraphs: a blank line where the step is well over the usual
    {
        Str l[] = {"one", "two", "three"};
        Str m[] = {"four", "five"};
        expect(t, "pdflayout.paragraphs", pdftone(&a,
               concat(&a, pdftlines(&a, 72, 700, 14, l, 3), pdftlines(&a, 72, 640, 14, m, 2))),
               "one\ntwo\nthree\n\nfour\nfive\n");
    }

    // Long lines wrap at the first space past PDF_LINEWRAP, and a long word
    // anywhere under PDF_LINEHARD, nothing lost
    {
        Buf w(&a, 4096);
        for (i32 i = 0; i < 400; i++) {
            if (i) putbyte(&w, ' ');
            putbyte(&w, 'w');
            printpad(&w, i, 4);
        }
        Str words = finish(&w);
        Str s = concat(&a, concat(&a, "BT /F1 2 Tf 5 Tz 10 700 Td (", words), ") Tj ET");
        Str got = pdftone(&a, s);
        b32 ok = 1;
        Buf j(&a, 4096);
        i32 n = 0;
        for (Cut c = cut(got, '\n'); c.ok; c = cut(c.tail, '\n'), n++) {
            ok &= c.head.len>=PDF_LINEWRAP-6 || !c.tail.len;  // a word past it
            ok &= c.head.len <= PDF_LINEWRAP+5;
            if (n) putbyte(&j, ' ');
            print(&j, c.head);
        }
        expect(t, "pdflayout.wrap", istr(&a, ok && n==3), "1");
        expect(t, "pdflayout.wrap text", finish(&j), words);

        Buf x(&a, 4096);
        for (i32 i = 0; i < 3000; i++) putbyte(&x, 'x');
        got = pdftone(&a, concat(&a, concat(&a, "BT /F1 1 Tf 2 Tz 10 700 Td (", finish(&x)),
                                 ") Tj ET"));
        Buf lens(&a, 64);
        for (Cut c = cut(got, '\n'); c.ok; c = cut(c.tail, '\n')) {
            print(&lens, c.head.len);
            putbyte(&lens, ' ');
        }
        expect(t, "pdflayout.hardwrap", finish(&lens), "1488 1488 24 ");
    }

    // Form fields at their places: the name up the parents, the value
    // inherited; a checkbox in its state; a choice's values; push buttons,
    // unchecked boxes and signatures left out
    {
        Str extra[] = {
            "<< /FT /Tx /T (person) /V (Ada Lovelace) /Kids [4 0 R] >>",
            "<< /Type /Annot /Subtype /Widget /Parent 3 0 R /T (name) /Rect [200 595 400 615] >>",
            "<< /Subtype /Widget /FT /Btn /T (agree) /V /Yes /AS /Yes /Rect [200 555 220 575] >>",
            "<< /Subtype /Widget /FT /Btn /Ff 65536 /T (go) /V /Yes /Rect [300 555 340 575] >>",
            "<< /Subtype /Widget /FT /Btn /T (no) /V /Off /AS /Off /Rect [400 555 420 575] >>",
            "<< /Subtype /Widget /FT /Sig /T (sig) /V (x) /Rect [72 400 200 420] >>",
            "<< /Subtype /Widget /FT /Ch /T (pick) /V [(red) (blue)] /Rect [200 515 400 535] >>",
        };
        expect(t, "pdflayout.fields", pdftpage(&a,
               "BT /F1 12 Tf 72 600 Td (Name:) Tj 0 -40 Td (Agree:) Tj 0 -40 Td (Colours:) Tj ET",
               PDFT_HELV, extra, countof(extra),
               "/Annots [4 0 R 5 0 R 6 0 R 7 0 R 8 0 R 9 0 R]"),
               "Name:  [field person.name: Ada Lovelace]\nAgree:  [field agree: Yes]\n"
               "Colours:  [field pick: red, blue]\n");
    }

    // Annotations' text after the page's: links and popups left out, an
    // odd subtype's name cleaned, a missing one skipped
    {
        Str extra[] = {
            "<< /Type /Annot /Subtype /Text /Rect [72 400 92 420] /Contents (A note) >>",
            "<< /Type /Annot /Subtype /FreeText /Rect [72 300 400 330] "
                "/Contents (Typed\\non   top) >>",
            "<< /Type /Annot /Subtype /Square /Rect [0 0 10 10] /Contents (Boxed) >>",
            "<< /Type /Annot /Subtype /Link /Rect [0 0 10 10] /Contents (no) >>",
            "<< /Type /Annot /Subtype /Popup /Contents (no) >>",
            "<< /Type /Annot /Subtype /In#28k /Contents <FEFF00480069> >>",
        };
        expect(t, "pdflayout.annots", pdftpage(&a,
               "BT /F1 12 Tf 72 700 Td (Body.) Tj ET", PDFT_HELV, extra, countof(extra),
               "/Annots [3 0 R 4 0 R 5 0 R 6 0 R 7 0 R 8 0 R 99 0 R]"),
               "Body.\n[comment: A note]\n[text box: Typed on top]\n"
               "[annotation Square: Boxed]\n[annotation Ink: Hi]\n");
    }

    // The head: the title, then the outline, indented by level, each
    // entry with its page, by /Dest or a GoTo to a named destination; a
    // /Next back to the first and a /First to itself end their chains
    {
        Str extra[] = {
            "<< /Title (Report) >>",
            "<< /Type /Outlines /First 5 0 R >>",
            "<< /Title (Alpha) /First 7 0 R /Next 6 0 R /Dest [10 0 R /Fit] >>",
            "<< /Title <FEFF0042006500740061> /Next 5 0 R /A << /S /GoTo /D (beta) >> >>",
            "<< /Title (Child) /First 7 0 R /Dest [11 0 R /XYZ 0 0 0] >>",
            "<< /Kids [9 0 R] >>",
            "<< /Limits [(a) (c)] /Names [(alpha) [10 0 R /Fit] (beta) [11 0 R /Fit]] >>",
        };
        Str pages[] = {"BT /F1 12 Tf 72 700 Td (One.) Tj ET", "BT /F1 12 Tf 72 700 Td (Two.) Tj ET"};
        PdfOut r = pdftread(&a, pdftdoc(&a, PDFT_HELV, pages, 2, extra, countof(extra), {},
                                        "/Outlines 4 0 R /Names << /Dests 8 0 R >>", PDFB_INFO));
        expect(t, "pdflayout.outline", r.text,
               "[title: Report]\n[outline: 3 entries]\n  Alpha (page 1)\n    Child (page 2)\n"
               "  Beta (page 2)\n--- page 1 ---\nOne.\n--- page 2 ---\nTwo.\n");
        expect(t, "pdflayout.outline valid", pdfoutbad(&r, &a), {});
    }

    // At most PDF_OUTLINES entries, the rest noted
    {
        i32  n     = PDF_OUTLINES + 3;
        Str *extra = alloc<Str>(&a, n+1);
        extra[0] = "<< /Type /Outlines /First 4 0 R >>";
        for (i32 i = 0; i < n; i++) {
            Buf b(&a, 64);
            print(&b, "<< /Title (e");
            print(&b, (i64)i);
            print(&b, ")");
            if (i+1 < n) {
                print(&b, " /Next ");
                print(&b, (i64)(5 + i));
                print(&b, " 0 R");
            }
            print(&b, " >>");
            extra[1+i] = finish(&b);
        }
        Str    page = "BT /F1 12 Tf 72 700 Td (One.) Tj ET";
        PdfOut r    = pdftread(&a, pdftdoc(&a, PDFT_HELV, &page, 1, extra, n+1, {},
                                           "/Outlines 3 0 R"));
        Cut    c    = cut(r.text, '\n');
        expect(t, "pdflayout.outlines head", c.head, "[outline: 1000 entries]");
        iz     at   = find(r.text, "[outline: more entries not shown]\n--- page 1 ---\n");
        expect(t, "pdflayout.outlines more", istr(&a, at > 0), "1");
        expect(t, "pdflayout.outlines last", istr(&a, find(r.text, "  e999\n") > 0), "1");
    }
}

// Images shown by placement (pdfimage) and picked for a window (pdfpick)

// An image shown: its mime, size and first and last pixels as RGB, or
// "fail: why", and "(perm moved)" if a failure touched perm.
static Str pdftshow(Arena *a, Str pdf, PdfPlace *pl, i64 edge = 4000, i64 pixels = 1<<24,
                    iz maxb64 = 5<<20, PdfShown *out = 0)
{
    Arena    perm = carve(a, 4<<20);
    byte    *beg  = perm.beg;
    PdfShown s    = pdfimage(&perm, *a, pdf, pl, edge, pixels, maxb64);
    if (out) {
        *out = s;
    }
    if (!s.data.data) {
        return concat(a, concat(a, "fail: ", s.err), perm.beg==beg ? Str("") : Str(" (perm moved)"));
    }
    Image p = imgdecode(a, s.data, 1<<24);
    if (!p.rgba || p.width!=s.w || p.height!=s.h) {
        return "undecodable";
    }
    Buf b(a, 64);
    print(&b, s.mime);
    putbyte(&b, ' ');
    print(&b, (i64)s.w);
    putbyte(&b, 'x');
    print(&b, (i64)s.h);
    putbyte(&b, ' ');
    iz last = (iz)s.w*s.h - 1;
    for (i32 c = 0; c < 3; c++) printhex(&b, p.rgba[c], 2);
    putbyte(&b, ' ');
    for (i32 c = 0; c < 3; c++) printhex(&b, p.rgba[4*last+c], 2);
    return finish(&b);
}

// Raw samples, w by h of n bytes a pixel: left of half, l, else r.
static Str pdfthalves(Arena *a, i32 w, i32 h, Str l, Str r)
{
    Buf b(a, (iz)w*h*l.len);
    for (i32 y = 0; y < h; y++) {
        for (i32 x = 0; x < w; x++) {
            print(&b, x<w/2 ? l : r);
        }
    }
    return finish(&b);
}

// A document of every kind of image pdfimage shows, and its placements:
// on page 1, XObjects of RGB, RGB with a soft mask, a stencil painted
// red, JPEG 2000, a JPEG, an RGB one with a /Mask stencil, gray inverted
// by Decode, and a colour key; on page 2, an inline image in a named
// Indexed space (its palette a stream), a form holding an inline
// image, and fax (G4). Soft mask and palette are deflated, so that
// decoding them takes memory.
static Str pdftimages(Arena *a)
{
    Str rgb   = pdfthalves(a, 32, 24, Str((u8 *)"\xff\x00\x00", 3), Str((u8 *)"\x00\x00\xff", 3));
    Str green = pdfthalves(a, 32, 24, Str((u8 *)"\x00\xff\x00", 3), Str((u8 *)"\x00\xff\x00", 3));
    Str alpha = pdfthalves(a, 32, 24, Str((u8 *)"\x00", 1), Str((u8 *)"\xff", 1));
    Str mask  = pdfthalves(a, 32, 24, Str((u8 *)"\x00", 1), Str((u8 *)"\xff", 1));  // 1 bpc: 4 bytes a row
    mask.len /= 8;
    Str gray  = pdfthalves(a, 32, 24, Str((u8 *)"\x00", 1), Str((u8 *)"\xff", 1));
    Str keyed = pdfthalves(a, 32, 24, Str((u8 *)"\x10", 1), Str((u8 *)"\x80", 1));

    Buf inl(a, 4096);
    print(&inl, "q 100 0 0 100 72 600 cm BI /W 24 /H 24 /CS /CS0 /BPC 8 /F /AHx ID\n");
    for (i32 i = 0; i < 24*24; i++) {
        print(&inl, i%24 < 12 ? Str("00") : Str("01"));
    }
    print(&inl, ">\nEI Q q /Fm Do Q q 300 0 0 30 72 100 cm /Fax Do Q");
    Buf form(a, 1024);
    print(&form, "q 50 0 0 50 300 300 cm BI /W 24 /H 24 /CS /G /BPC 8 ID ");
    for (i32 i = 0; i < 24*24; i++) {
        putbyte(&form, 0x80);
    }
    print(&form, " EI Q");

    Str extra[] = {
        pdfbstream(a, "/Type /XObject /Subtype /Image /Width 32 /Height 24 "
                   "/ColorSpace /DeviceRGB /BitsPerComponent 8", rgb),               // 3
        pdfbstream(a, "/Type /XObject /Subtype /Image /Width 32 /Height 24 "
                   "/ColorSpace /DeviceRGB /BitsPerComponent 8 /SMask 5 0 R", green),  // 4
        pdfbstream(a, "/Type /XObject /Subtype /Image /Width 32 /Height 24 "
                   "/ColorSpace /DeviceGray /BitsPerComponent 8 /Filter /FlateDecode",
                   pdfbzlib(a, alpha)),                                              // 5
        pdfbstream(a, "/Type /XObject /Subtype /Image /Width 32 /Height 24 "
                   "/ImageMask true /BitsPerComponent 1", mask),                     // 6
        pdfbstream(a, "/Type /XObject /Subtype /Image /Width 600 /Height 800 "
                   "/ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /JPXDecode", "jp"),  // 7
        pdfbstream(a, "/Type /XObject /Subtype /Image /Width 16 /Height 16 "
                   "/ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /DCTDecode",
                   Str((u8 *)fxbaseline, countof(fxbaseline)-1)),                    // 8
        pdfbstream(a, "/Type /XObject /Subtype /Form /BBox [0 0 612 792] "
                   "/Resources << >>", finish(&form)),                               // 9
        pdfbstream(a, "/Type /XObject /Subtype /Image /Width 2700 /Height 24 "
                   "/ColorSpace /DeviceGray /BitsPerComponent 1 /Decode [1 0] "
                   "/Filter /CCITTFaxDecode /DecodeParms << /K -1 /Columns 2700 "
                   "/BlackIs1 true >>", pihex(a, ccg4)),                             // 10
        pdfbstream(a, "/Type /XObject /Subtype /Image /Width 32 /Height 24 "
                   "/ColorSpace /DeviceRGB /BitsPerComponent 8 /Mask 12 0 R", green),  // 11
        pdfbstream(a, "/Type /XObject /Subtype /Image /Width 32 /Height 24 "
                   "/ImageMask true /BitsPerComponent 1 /Decode [1 0]", mask),       // 12
        pdfbstream(a, "/Type /XObject /Subtype /Image /Width 32 /Height 24 "
                   "/ColorSpace /DeviceGray /BitsPerComponent 8 /Decode [1 0]", gray),  // 13
        pdfbstream(a, "/Type /XObject /Subtype /Image /Width 32 /Height 24 "
                   "/ColorSpace /DeviceGray /BitsPerComponent 8 /Mask [0 32]", keyed),  // 14
        pdfbstream(a, "/Filter /FlateDecode",
                   pdfbzlib(a, Str((u8 *)"\xff\x00\x00\x00\xff\x00", 6))),             // 15
    };
    Str res = "<< /XObject << /Rgb 3 0 R /Sm 4 0 R /Mk 6 0 R /Jp 7 0 R /Dct 8 0 R "
              "/Fm 9 0 R /Fax 10 0 R /Sten 11 0 R /Inv 13 0 R /Key 14 0 R >> "
              "/ColorSpace << /CS0 [/Indexed /DeviceRGB 1 15 0 R] >> >>";
    Str pages[] = {
        "q 100 0 0 50 72 700 cm /Rgb Do Q q 100 0 0 50 72 600 cm /Sm Do Q "
        "1 0 0 rg q 100 0 0 50 72 500 cm /Mk Do Q q 100 0 0 50 72 400 cm /Jp Do Q "
        "q 100 0 0 50 72 300 cm /Dct Do Q q 100 0 0 50 72 200 cm /Sten Do Q "
        "q 100 0 0 50 72 100 cm /Inv Do Q q 100 0 0 50 72 50 cm /Key Do Q",
        finish(&inl),
    };
    return pdftdoc(a, res, pages, countof(pages), extra, countof(extra));
}

// The placement of an object, or for an inline image (objnum 0) the one
// in a page's own stream, or for a form's (objnum -1) that one.
static PdfPlace *pdftplace(PdfOut *r, i32 objnum)
{
    for (iz i = 0; i < r->places.len; i++) {
        PdfPlace *pl = r->places.data + i;
        b32 inl = pl->inlineoff >= 0;
        if ((objnum>0 && !inl && pl->objnum==objnum) ||
            (!objnum && inl && pl->stream>=0) || (objnum<0 && inl && pl->stream<0)) {
            return pl;
        }
    }
    return 0;
}

static PdfPlace pdftpl(i32 id, iz line, i32 w = 100, i32 h = 100)
{
    PdfPlace pl = {};
    pl.id     = id;
    pl.page   = 1;
    pl.line   = line;
    pl.objnum = id;
    pl.w      = w;
    pl.h      = h;
    pl.inlineoff = -1;
    return pl;
}

// The picks, as "at... last cut pixels maxb64".
static Str pdftpick(Arena *a, PdfPlace *v, iz n, iz first, iz last, i64 pixels = 1<<20)
{
    PdfPick k = pdfpick(*a, {v, n, n}, first, last, 1280, pixels, 5<<20);
    Buf     b(a, 128);
    for (i32 i = 0; i < k.n; i++) {
        print(&b, (i64)k.at[i]);
        putbyte(&b, ' ');
    }
    print(&b, "last=");
    print(&b, k.last);
    print(&b, k.cut ? Str(" cut ") : Str(" "));
    print(&b, k.pixels);
    putbyte(&b, ' ');
    print(&b, k.maxb64);
    return finish(&b);
}

// Big images, each with the whole test arena, so that what tests before
// them allocated does not decide whether they fit.
static void test_pdfshowbig(Test *t)
{
    Arena a = t->perm;

    // A big image in a small file: its samples decode past the budget a
    // file of this size (112 KB) has for its text, 28 MB, as inflating
    // them charges 36 MB, its growing guesses included
    {
        Arena s     = a;
        iz    n     = (iz)4200*4200;  // past PDF_DECODEDMIN
        Str   zeros = {alloc<u8>(&s, n), n};
        Str   img   = pdfbstream(&s, "/Type /XObject /Subtype /Image /Width 4200 /Height 4200 "
                                 "/ColorSpace /DeviceGray /BitsPerComponent 8 "
                                 "/Filter /FlateDecode", pdfbzlib(&s, zeros));
        Str   page  = "q 500 0 0 500 50 50 cm /Big Do Q";
        Str   big   = pdftdoc(&s, "<< /XObject << /Big 3 0 R >> >>", &page, 1, &img, 1);
        PdfOut rb   = pdftread(&s, big);
        expect(t, "pdfshow.big", rb.places.len ? pdftshow(&s, big, rb.places.data, 100, 1<<20) : "",
               "image/png 100x100 000000 000000");
    }

    // A soft mask past the decoded budget's share grows it; a small
    // image's soft mask of 4200 squared is scaled to the image
    {
        Str   str_pg = "q 50 0 0 50 50 50 cm /X Do Q";
        Arena s    = a;
        iz    n    = (iz)4200*4200;
        Str   ones = {alloc<u8>(&s, n), n};
        for (iz i = 0; i < n; i++) ones.data[i] = 0xff;
        Str zl = pdfbzlib(&s, ones);
        Str smx[] = {pdfbstream(&s, "/Type /XObject /Subtype /Image /Width 4200 /Height 4200 "
                                "/ColorSpace /DeviceGray /BitsPerComponent 8 /SMask 4 0 R "
                                "/Filter /FlateDecode", zl),
                     pdfbstream(&s, "/Type /XObject /Subtype /Image /Width 4200 /Height 4200 "
                                "/ColorSpace /DeviceGray /BitsPerComponent 8 /Filter /FlateDecode", zl)};
        Str   dd = pdftdoc(&s, "<< /XObject << /X 3 0 R >> >>", &str_pg, 1, smx, 2);
        PdfOut q = pdftread(&s, dd);
        expect(t, "pdfshow.big smask", q.places.len ? pdftshow(&s, dd, q.places.data, 100, 1<<20) : "",
               "image/png 100x100 ffffff ffffff");
    }
    {
        Arena s = a;
        iz    n = (iz)4200*4200;
        Str   o = {alloc<u8>(&s, n), n};
        for (iz i = 0; i < n; i++) o.data[i] = 0;
        Str grn = pdfthalves(&s, 32, 24, Str((u8 *)"\x00\xff\x00", 3), Str((u8 *)"\x00\xff\x00", 3));
        Str sx[] = {pdfbstream(&s, "/Type /XObject /Subtype /Image /Width 32 /Height 24 "
                               "/ColorSpace /DeviceRGB /BitsPerComponent 8 /SMask 4 0 R", grn),
                    pdfbstream(&s, "/Type /XObject /Subtype /Image /Width 4200 /Height 4200 "
                               "/ColorSpace /DeviceGray /BitsPerComponent 8 /Filter /FlateDecode",
                               pdfbzlib(&s, o))};
        Str   pg2 = "q 50 0 0 50 50 50 cm /X Do Q";
        Str   d2 = pdftdoc(&s, "<< /XObject << /X 3 0 R >> >>", &pg2, 1, sx, 2);
        PdfOut q2 = pdftread(&s, d2);
        expect(t, "pdfshow.big smask small image", q2.places.len ? pdftshow(&s, d2, q2.places.data) : "",
               "image/png 32x24 ffffff ffffff");
    }
}

static void test_pdfshow(Test *t)
{
    Arena  a   = t->perm;
    Str    pdf = pdftimages(&a);
    PdfOut r   = pdftread(&a, pdf);
    expect(t, "pdfshow.read", r.why, "");
    expect(t, "pdfshow.places", istr(&a, r.places.len), "11");

    // XObjects of each kind
    expect(t, "pdfshow.rgb", pdftshow(&a, pdf, pdftplace(&r, 3)), "image/png 32x24 ff0000 0000ff");
    expect(t, "pdfshow.rgb shrunk", pdftshow(&a, pdf, pdftplace(&r, 3), 16),
           "image/png 16x12 ff0000 0000ff");
    expect(t, "pdfshow.rgb pixels", pdftshow(&a, pdf, pdftplace(&r, 3), 4000, 12*16),
           "image/png 15x11 ff0000 0000ff");  // fitscale steps down by 3%
    expect(t, "pdfshow.smask", pdftshow(&a, pdf, pdftplace(&r, 4)), "image/png 32x24 ffffff 00ff00");
    expect(t, "pdfshow.stencil", pdftshow(&a, pdf, pdftplace(&r, 6)), "image/png 32x24 ff0000 ffffff");
    expect(t, "pdfshow.stencil fill", istr(&a, pdftplace(&r, 6)->fill[0]), "255");
    expect(t, "pdfshow.jpx", pdftshow(&a, pdf, pdftplace(&r, 7)), "fail: JPEG 2000 images are not shown");
    expect(t, "pdfshow.mask stencil", pdftshow(&a, pdf, pdftplace(&r, 11)),
           "image/png 32x24 ffffff 00ff00");
    expect(t, "pdfshow.decode", pdftshow(&a, pdf, pdftplace(&r, 13)), "image/png 32x24 ffffff 000000");
    expect(t, "pdfshow.colour key", pdftshow(&a, pdf, pdftplace(&r, 14)), "image/png 32x24 ffffff 808080");

    // A JPEG passed through as it is, or decoded when too big
    {
        PdfShown s = {};
        Str      d = pdftshow(&a, pdf, pdftplace(&r, 8), 4000, 1<<24, 5<<20, &s);
        expect(t, "pdfshow.jpeg", cut(d, ' ').head, "image/jpeg");
        Str fx = {(u8 *)fxbaseline, countof(fxbaseline)-1};
        expect(t, "pdfshow.jpeg same", istr(&a, s.data==fx), "1");
        d = pdftshow(&a, pdf, pdftplace(&r, 8), 8);
        expect(t, "pdfshow.jpeg shrunk", takehead(d, 13), "image/png 8x8");
    }

    // Inline images: in the page's stream, its space named in the page's
    // resources, and in a form's
    expect(t, "pdfshow.inline", pdftshow(&a, pdf, pdftplace(&r, 0)), "image/png 24x24 ff0000 00ff00");
    expect(t, "pdfshow.inline form", pdftshow(&a, pdf, pdftplace(&r, -1)),
           "image/png 24x24 808080 808080");
    expect(t, "pdfshow.inline form objnum", istr(&a, pdftplace(&r, -1)->objnum), "9");
    {
        PdfPlace pl = *pdftplace(&r, 0);
        pl.inlineoff++;
        expect(t, "pdfshow.inline off", pdftshow(&a, pdf, &pl),
               "fail: its content stream does not hold it");
        pl.inlineoff = 1<<20;
        expect(t, "pdfshow.inline past", pdftshow(&a, pdf, &pl),
               "fail: its content stream does not hold it");
    }

    // Fax, G4 with BlackIs1 and Decode [1 0]: black where the pattern is
    {
        PdfShown s = {};
        Str      d = pdftshow(&a, pdf, pdftplace(&r, 10), 4000, 1<<24, 5<<20, &s);
        expect(t, "pdfshow.fax", takehead(d, 19), "image/png 2700x24 f");
        Image p     = imgdecode(&a, s.data, 1<<24);
        iz    wrong = !p.rgba;
        for (i32 y = 0; p.rgba && y < CCROWS; y++) {
            for (i32 x = 0; x < CCCOLS; x++) {
                u8 g = p.rgba[((iz)y*CCCOLS + x)*4];
                wrong += ccpattern(x, y) ? g!=0 : g!=255;
            }
        }
        expect(t, "pdfshow.fax pixels", istr(&a, wrong), "0");
    }

    {
        {
            // 24 rows of a white run of 24, Modified Huffman without EOLs
            u8 mh[21] = {};
            for (i32 b = 0; b < 24*7; b++) {
                if ((0x28 >> (6 - b%7)) & 1) mh[b/8] |= (u8)(0x80 >> (b%8));
            }
            Str fx[] = {pdfbstream(&a, "/Type /XObject /Subtype /Image /Width 24 /Height 24 "
                                   "/ColorSpace /DeviceGray /BitsPerComponent 1 "
                                   "/Filter /CCITTFaxDecode /DecodeParms << /K 0 /Columns 24 >>",
                                   Str(mh, 21))};
            Str   pg3 = "q 50 0 0 50 50 50 cm /X Do Q";
            Str   d3 = pdftdoc(&a, "<< /XObject << /X 3 0 R >> >>", &pg3, 1, fx, 1);
            PdfOut q3 = pdftread(&a, d3);
            expect(t, "pdfshow.ccitt mh no eol", pdftshow(&a, d3, q3.places.data), "image/png 24x24 ffffff ffffff");
        }
        Arena b = a;
        auto one = [&](Str page, Str res, Str *ex, i32 nex) -> Str {
            Str    d = pdftdoc(&b, res, &page, 1, ex, nex);
            PdfOut q = pdftread(&b, d);
            return q.places.len ? pdftshow(&b, d, q.places.data) : Str("no place");
        };
        Str xo  = "<< /XObject << /X 3 0 R >> >>";
        Str pg  = "q 50 0 0 50 50 50 cm /X Do Q";
        Str cmyk = pdfthalves(&b, 24, 24, Str((u8 *)"\x00\x00\x00\x00", 4), Str((u8 *)"\x00\x00\x00\xff", 4));
        Str rb   = pdfthalves(&b, 24, 24, Str((u8 *)"\xff\x00\x00", 3), Str((u8 *)"\x00\x00\xff", 3));
        Str icc4[] = {pdfbstream(&b, "/Type /XObject /Subtype /Image /Width 24 /Height 24 "
                                 "/ColorSpace [/ICCBased 4 0 R] /BitsPerComponent 8", cmyk),
                      pdfbstream(&b, "/N 4", "icc")};
        expect(t, "pdfshow.icc n4", one(pg, xo, icc4, 2), "image/png 24x24 ffffff 000000");
        Str iccalt[] = {pdfbstream(&b, "/Type /XObject /Subtype /Image /Width 24 /Height 24 "
                                   "/ColorSpace [/ICCBased 4 0 R] /BitsPerComponent 8", rb),
                        pdfbstream(&b, "/Alternate /DeviceRGB", "icc")};
        expect(t, "pdfshow.icc alternate", one(pg, xo, iccalt, 2), "image/png 24x24 ff0000 0000ff");
        Str lab = pdfthalves(&b, 24, 24, Str((u8 *)"\xff\x80\x80", 3), Str((u8 *)"\x00\x80\x80", 3));
        Str labx[] = {pdfbstream(&b, "/Type /XObject /Subtype /Image /Width 24 /Height 24 "
                                 "/ColorSpace [/Lab << /WhitePoint [0.9505 1 1.089] >>] "
                                 "/BitsPerComponent 8", lab)};
        expect(t, "pdfshow.lab default decode", one(pg, xo, labx, 1), "image/png 24x24 fffffe 000000");
        Str idx = pdfthalves(&b, 24, 24, Str((u8 *)"\x00", 1), Str((u8 *)"\x05", 1));
        Str idxx[] = {pdfbstream(&b, "/Type /XObject /Subtype /Image /Width 24 /Height 24 "
                                 "/ColorSpace [/Indexed /DeviceRGB 0 <ff0000>] /BitsPerComponent 8", idx)};
        expect(t, "pdfshow.indexed past hival", one(pg, xo, idxx, 1), "image/png 24x24 ff0000 ff0000");
        Buf fm(&b, 2048);
        print(&fm, "q 50 0 0 50 300 300 cm BI /W 24 /H 24 /CS /CF /BPC 8 ID ");
        for (i32 i = 0; i < 24*24; i++) putbyte(&fm, i%24 < 12 ? 0 : 1);
        print(&fm, " EI Q");
        Str formx[] = {pdfbstream(&b, "/Type /XObject /Subtype /Form /BBox [0 0 612 792] "
                                  "/Resources << /ColorSpace << /CF [/Indexed /DeviceRGB 1 "
                                  "<ff000000ff00>] >> >>", finish(&fm))};
        expect(t, "pdfshow.inline in form named space", one(pg, xo, formx, 1),
               "image/png 24x24 ff0000 00ff00");
        Str g4  = pdfbstream(&b, "/Type /XObject /Subtype /Image /Width 2700 /Height 24 "
                             "/ColorSpace /DeviceGray /BitsPerComponent 1 /Decode [1 0] "
                             "/Filter /CCITTFaxDecode /DecodeParms << /K -1 /Columns 2700 "
                             "/BlackIs1 true >>", pihex(&b, ccg4));
        Str want = one(pg, xo, &g4, 1);
        Str nocol = pdfbstream(&b, "/Type /XObject /Subtype /Image /Width 2700 /Height 24 "
                               "/ColorSpace /DeviceGray /BitsPerComponent 1 /Decode [1 0] "
                               "/Filter /CCITTFaxDecode /DecodeParms << /K -1 "
                               "/BlackIs1 true >>", pihex(&b, ccg4));
        expect(t, "pdfshow.ccitt no columns", one(pg, xo, &nocol, 1), want);
        Str g3 = pdfbstream(&b, "/Type /XObject /Subtype /Image /Width 2700 /Height 24 "
                            "/ColorSpace /DeviceGray /BitsPerComponent 1 /Decode [1 0] "
                            "/Filter /CCITTFaxDecode /DecodeParms << /K 0 /Columns 2700 "
                            "/BlackIs1 true >>", pihex(&b, ccg31d));
        expect(t, "pdfshow.ccitt g3 via pdfimage", one(pg, xo, &g3, 1), want);
        Str g32 = pdfbstream(&b, "/Type /XObject /Subtype /Image /Width 2700 /Height 24 "
                             "/ColorSpace /DeviceGray /BitsPerComponent 1 /Decode [1 0] "
                             "/Filter /CCITTFaxDecode /DecodeParms << /K 1 /Columns 2700 "
                             "/EncodedByteAlign true /BlackIs1 true >>", pihex(&b, ccg32dfill));
        expect(t, "pdfshow.ccitt aligned via pdfimage", one(pg, xo, &g32, 1), want);
        expect(t, "pdfshow.inline EI in data", one(
               "q 50 0 0 50 50 50 cm BI /W 4 /H 1 /CS /G /BPC 8 ID  EI \nEI Q", "<< >>", 0, 0),
               "image/png 4x1 202020 202020");
        {
            Arena s2 = b;
            Str lr = pdfthalves(&s2, 24, 24, Str((u8 *)"\xff\xff\xff", 3), Str((u8 *)"\xff\xff\xff", 3));
            Str lx[] = {pdfbstream(&s2, "/Type /XObject /Subtype /Image /Width 24 /Height 24 "
                                   "/ColorSpace [/Lab << /WhitePoint [0.9505 1 1.089] /Range [-10 10 -10 10] >>] "
                                   "/BitsPerComponent 8", lr)};
            Str   d3 = pdftdoc(&s2, xo, &pg, 1, lx, 1);
            PdfOut q3 = pdftread(&s2, d3);
            expect(t, "pdfshow.lab range", pdftshow(&s2, d3, q3.places.data), "image/png 24x24 fff8ec fff8ec");
            Str g32 = pdfbstream(&s2, "/Type /XObject /Subtype /Image /Width 2700 /Height 24 "
                                 "/ColorSpace /DeviceGray /BitsPerComponent 1 /Decode [1 0] "
                                 "/Filter /CCITTFaxDecode /DecodeParms << /K 1 /Columns 2700 "
                                 "/BlackIs1 true >>", pihex(&s2, ccg32d));
            Str   d4 = pdftdoc(&s2, xo, &pg, 1, &g32, 1);
            PdfOut q4 = pdftread(&s2, d4);
            expect(t, "pdfshow.ccitt g3 2d via pdfimage", pdftshow(&s2, d4, q4.places.data), want);
        }
        PdfPlace v[] = {pdftpl(1, 10, 100, 23)};
        expect(t, "pdfpick.short", pdftpick(&a, v, 1, 1, 100), "last=100 1048576 5242880");
    }

    // Failures leave perm as it was, with a reason
    {
        PdfPlace pl = *pdftplace(&r, 3);
        pl.objnum = 99;
        expect(t, "pdfshow.no object", pdftshow(&a, pdf, &pl), "fail: its object is not in the file");
        pl = *pdftplace(&r, 3);
        expect(t, "pdfshow.too big", pdftshow(&a, pdf, &pl, 4000, 1<<24, 8),
               "fail: the image is too large, and does not shrink to fit");
        expect(t, "pdfshow.not pdf", pdftshow(&a, "<html>", &pl), "fail: it is not a PDF");
        i32  nb = 0;
        Str *ob = pdfbdoc(&a, &nb);
        expect(t, "pdfshow.encrypted", pdftshow(&a, pdfbuild(&a, ob, nb, PDFB_ENCRYPT), &pl),
               "fail: it is encrypted");
        Str page = "q 9 0 0 9 0 0 cm /X Do Q";
        Str pat  = "<< /Type /XObject /Subtype /Image /Width 2 /Height 2 "
                   "/ColorSpace /Pattern /BitsPerComponent 8 /Length 4 >>\nstream\nabcd\nendstream";
        Str bad  = pdftdoc(&a, "<< /XObject << /X 3 0 R >> >>", &page, 1, &pat, 1);
        PdfOut rb = pdftread(&a, bad);
        expect(t, "pdfshow.pattern", pdftshow(&a, bad, rb.places.data), "fail: an unsupported colour space");
    }

    // In scratch of any size, out of memory or exactly the full result
    {
        PdfPlace *pls[] = {pdftplace(&r, 4), pdftplace(&r, 10), pdftplace(&r, 0), pdftplace(&r, 8)};
        i32 bad = 0;
        i32 oom = 0;
        i32 ok  = 0;
        for (i32 k = 0; k < countof(pls); k++) {
            Arena    s    = a;
            Arena    perm = carve(&s, 1<<20);
            PdfShown full = pdfimage(&perm, s, pdf, pls[k], 200, 1<<20, 5<<20);
            bad += !full.data.data;
            for (iz n = 1<<12; n < 8<<20; n += n/8 + 1) {
                Arena    sc = s;
                Arena    pm = carve(&sc, 1<<20);
                byte    *beg = pm.beg;
                sc.end = sc.beg + n;
                PdfShown x  = pdfimage(&pm, sc, pdf, pls[k], 200, 1<<20, 5<<20);
                if (!x.data.data && find(x.err, "out of memory")>=0 && pm.beg==beg) {
                    oom++;
                } else if (x.data.data && x.data==full.data && x.mime==full.mime) {
                    ok++;
                } else {
                    bad++;
                    if (bad == 1) {
                        expect(t, "pdfshow.memsweep first", concat(&a, istr(&a, n), x.err), "");
                    }
                }
            }
        }
        expect(t, "pdfshow.memsweep", istr(&a, bad), "0");
        expect(t, "pdfshow.memsweep both", istr(&a, oom>0 && ok>0), "1");
    }

    // Picks: by line, in the window, each object once, a stencil once a
    // colour, small and undecodable ones left out
    {
        PdfPlace v[] = {pdftpl(3, 30), pdftpl(1, 10), pdftpl(2, 20), pdftpl(1, 40),
                        pdftpl(4, 50, 23, 100), pdftpl(5, 60), pdftpl(6, 70)};
        v[5].image = PDFI_JPX;
        v[6].image = PDFI_JBIG2;
        expect(t, "pdfpick.order", pdftpick(&a, v, countof(v), 1, 100), "1 2 0 last=100 1048576 3495253");
        expect(t, "pdfpick.window", pdftpick(&a, v, countof(v), 15, 35), "2 0 last=35 1048576 5242880");
        expect(t, "pdfpick.none", pdftpick(&a, v, countof(v), 41, 100), "last=100 1048576 5242880");
        // With none in the window, the limits are still within the shares
        expect(t, "pdfpick.none wide", pdftpick(&a, v, countof(v), 41, 100, 8<<20),
               "last=100 4194304 5242880");
        expect(t, "pdfpick.share", pdftpick(&a, v, countof(v), 1, 100, 4<<20), "1 2 0 last=100 1398101 3495253");

        PdfPlace m[] = {pdftpl(1, 10), pdftpl(1, 10), pdftpl(1, 10), pdftpl(2, 20), pdftpl(2, 30)};
        m[0].mask = m[1].mask = m[2].mask = 1;
        m[1].fill[0] = 255;
        m[3].fill[0] = 255;  // not a stencil: the fill does not count
        expect(t, "pdfpick.stencils", pdftpick(&a, m, countof(m), 1, 100), "0 1 3 last=100 1048576 3495253");
    }

    // Past PDF_IMAGES: the window cut before the next image's line, its
    // duplicates and small ones passed over; but not before a line that
    // holds a picked stencil
    {
        PdfPlace v[12] = {};
        for (i32 i = 0; i < 12; i++) {
            v[i] = pdftpl(i+1, 10*(i+1));
        }
        v[8] = pdftpl(1, 90);       // a duplicate
        v[9] = pdftpl(99, 95, 10);  // small
        expect(t, "pdfpick.cut", pdftpick(&a, v, 12, 1, 200, 8<<20),
               "0 1 2 3 4 5 6 7 last=109 cut 524288 1310720");
        expect(t, "pdfpick.cut next", pdftpick(&a, v, 12, 110, 200, 8<<20),
               "10 11 last=200 2097152 5242880");
        for (i32 i = 0; i < 9; i++) {
            v[i] = pdftpl(1, 10);
            v[i].mask    = 1;
            v[i].fill[0] = (u8)i;
        }
        expect(t, "pdfpick.cut stencils", pdftpick(&a, v, 9, 1, 200),
               "0 1 2 3 4 5 6 7 last=200 524288 1310720");
        Arena tiny = a;
        tiny.end = tiny.beg + 64;
        expect(t, "pdfpick.no room", istr(&a, pdfpick(tiny, {v, 9, 9}, 1, 200, 1, 1, 1).n), "0");

        // More stencils on the last pick's line than fit: those are not
        // shown, and the window is cut before the next line with an image
        PdfPlace w[10] = {};
        for (i32 i = 0; i < 9; i++) {
            w[i] = pdftpl(1, 10);
            w[i].mask    = 1;
            w[i].fill[0] = (u8)i;
        }
        w[9] = pdftpl(2, 20);
        expect(t, "pdfpick.same line then later", pdftpick(&a, w, 10, 1, 200),
               "0 1 2 3 4 5 6 7 last=19 cut 524288 1310720");
    }
}

// readpdf remembers a PDF read in part (pdfmemo) in session perm: never
// past what is left of it, whatever the placements' labels need, and
// never more than PDF_MEMOBYTES in all, however many versions are read.
static Session *pdftmemosession(Arena *a, iz perm)
{
    Engine *e = alloc<Engine>(a);
    e->os       = newos(a, 24<<20);
    e->out      = newmailbox(a, 16<<20, 0, 0);
    e->statedir = "/state";
    Session *s = alloc<Session>(a);
    s->e         = e;
    s->sid       = "20260101-000000-aaaa";
    s->dir       = "/state/sessions/20260101-000000-aaaa";
    s->outdir    = "/state/sessions/20260101-000000-aaaa/out";
    s->log.os    = e->os;
    s->log.fd    = -1;
    s->guard.cwd = "/w";
    s->mc.nopdf  = 1;
    s->perm      = carve(a, perm);
    Block *c = alloc<Block>(a);
    c->type  = BLOCK_CALL;
    c->id    = "c1";
    c->name  = "read";
    c->args  = "{}";
    s->calls = {c, 1, 1};
    return s;
}

static void testreadpdfmemo(Test *t)
{
    Arena a = t->perm;

    // Two pages, each drawing a 2x2 image 3000 times
    Buf c(&a, 1<<17);
    print(&c, "BT /F1 12 Tf 72 700 Td (Many images) Tj ET\n");
    for (i32 k = 0; k < 3000; k++) {
        print(&c, "q 4 0 0 4 ");
        print(&c, (i64)(k%60*9));
        print(&c, " ");
        print(&c, (i64)(k/60*12));
        print(&c, " cm /Im Do Q\n");
    }
    Str content[2] = {};
    content[0] = finish(&c);
    content[1] = content[0];
    Str img = pdfbstream(&a, "/Type /XObject /Subtype /Image /Width 2 /Height 2 "
                             "/ColorSpace /DeviceRGB /BitsPerComponent 8",
                         Str{(u8 *)"\1\2\3\4\5\6\7\10\11\12\13\14", 12});
    Str res = "<< /Font << /F1 << /Type /Font /Subtype /Type1 /BaseFont /Helvetica "
              ">> >> /XObject << /Im 3 0 R >> >>";
    Str pdf = pdftdoc(&a, res, content, 2, &img, 1);
    Args args = {};
    args.limit = 2;  // short of the whole: remembered

    // Every perm size around what remembering it takes: no trap
    i32 tried = 0;
    for (iz room = 256<<10; room < 2<<20; room += 16<<10) {
        Arena    b = a;
        Session *s = pdftmemosession(&b, room);
        VFile   *f = vfind(s->e->os, "/w/m.pdf", 1);
        f->data   = pdf;
        f->exists = 1;
        f->mtime  = 1;
        Arena scratch = b;
        tried += readpdf(s, scratch, "/w/m.pdf", &args);
    }
    expect(t, "readpdf memo in a tight perm", istr(&a, tried), "112");

    // Many versions read: what is remembered stays under the budget
    Session *s = pdftmemosession(&a, 48<<20);
    VFile   *f = vfind(s->e->os, "/w/m.pdf", 1);
    f->data   = pdf;
    f->exists = 1;
    byte *before = s->perm.beg;
    for (i32 v = 1; v <= 60; v++) {
        f->mtime = v;
        s->callidx = 0;
        readpdf(s, a, "/w/m.pdf", &args);
        mailtake(s->e->out);
        for (VFile *o = s->e->os->files; o; o = o->next) {
            o->data = startswith(o->path, s->outdir) ? Str{} : o->data;
        }
    }
    iz used = s->perm.beg - before;
    expect(t, "readpdf memo budget", istr(&a, used < (iz)16<<20), "1");
}

// A PDF attached whose text, as a request shows it, has no image lines
// is not parsed again when the prompt is sent (pdfattachimages).
static void testpdfattachnoimages(Test *t)
{
    Arena    a   = t->perm;
    Session *s   = pdftmemosession(&a, 1<<20);
    Str      c   = "BT /F1 12 Tf 72 700 Td (Only words.) Tj ET";
    Str      pdf = pdftdoc(&a, PDFT_HELV, &c, 1);
    VFile *f = vfind(s->e->os, "/state/staged", 1);
    f->data   = pdf;
    f->exists = 1;
    AttachedFile af = attachfile(s->e, &a, s->sid, "/state/staged", "w.pdf");
    s->mc.novision = 0;
    i64 before = g_reserves;
    Slice<MsgImage> imgs = pdfattachimages(s, &s->perm, a, {}, af.file, 10);
    expect(t, "pdfattachimages none shown", istr(&a, (g_reserves-before)*10 + imgs.len), "0");
}
