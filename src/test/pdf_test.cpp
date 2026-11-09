// PDF tests: telling a PDF by its first bytes, and counting its pages
// by the root's /Count, by its page objects when it has none, and in
// object streams (deflated by png.cpp's encoder, as Wuffs inflates
// them), and spotting encryption; damaged files count none. Then the
// reader: its lexer and objects, printed canonically, its limits on
// nesting, and its charged allocation failing softly in small arenas.

// An object stream's dictionary and data, its DEFLATE wrapped as zlib
// is (a header, and a trailer the scan never reads), parms added to the
// dictionary.
static Str pdfobjstm(Arena *a, Str objs, Str parms = {})
{
    iz  cap = objs.len*9/8 + 16;
    u8 *out = alloc<u8>(a, cap);
    iz  n   = zdeflate(*a, out, cap, objs.data, objs.len);
    Buf b(a, 256 + n);
    print(&b, "5 0 obj\n<< /Type /ObjStm /N 1 /First 4 /Filter /FlateDecode ");
    print(&b, parms);
    print(&b, " >>\nstream\r\n\x78\x01");
    print(&b, Str(out, n));
    print(&b, "\0\0\0\0\nendstream\nendobj\n");
    return finish(&b);
}

static Str pdfpagesof(Arena *a, Str pdf)
{
    PdfInfo p = pdfinfo(*a, pdf);
    return concat(a, concat(a, istr(a, p.pages), p.encrypted ? Str(" encrypted") : Str{}),
                  pdfsniff(pdf) ? Str{} : Str(" not a PDF"));
}

static void test_pdf(Test *t)
{
    Arena a = t->perm;

    // Its header, which may come after a little junk
    expect(t, "pdf.sniff", istr(&a, pdfsniff("%PDF-1.7\n")), "1");
    expect(t, "pdf.sniff short", istr(&a, pdfsniff("%PDF")), "0");
    expect(t, "pdf.sniff png", istr(&a, pdfsniff("\x89PNG\r\n\x1a\n")), "0");
    Buf junk(&a, 4096);
    for (i32 i = 0; i < 1000; i++) {
        putbyte(&junk, ' ');
    }
    print(&junk, "%PDF-1.4");
    expect(t, "pdf.sniff junk", istr(&a, pdfsniff(finish(&junk))), "1");
    Buf far(&a, 4096);
    for (i32 i = 0; i < 1100; i++) {
        putbyte(&far, ' ');
    }
    print(&far, "%PDF-1.4");
    expect(t, "pdf.sniff far", istr(&a, pdfsniff(finish(&far))), "0");

    // The root's /Count, its own and not a nested dictionary's, over the
    // page objects, which an incremental update may repeat
    expect(t, "pdf.count", pdfpagesof(&a,
           "%PDF-1.4\n2 0 obj\n<< /Type /Pages /Kids [3 0 R 4 0 R] "
           "/Foo << /Count 99 >> /Count 2 >>\nendobj\n"
           "3 0 obj\n<</Type/Page/Parent 2 0 R>>\nendobj\n"
           "4 0 obj\n<</Type/Page/Parent 2 0 R>>\nendobj\n"
           "4 0 obj\n<</Type/Page/Parent 2 0 R/Annots[]>>\nendobj\n"), "2");
    expect(t, "pdf.count tight", pdfpagesof(&a,
           "%PDF-1.4\n<</Count 12/Type/Pages/Kids[]>>"), "12");
    expect(t, "pdf.count nested", pdfpagesof(&a,
           "%PDF-1.4\n<< /Type /Pages /Count 40 >> "
           "<< /Type /Pages /Parent 2 0 R /Count 7 >>"), "40");
    expect(t, "pdf.objects", pdfpagesof(&a,
           "%PDF-1.4\n<< /Type /Page >> << /Type\n/Page>> << /Type /PageLabels >>"), "2");
    expect(t, "pdf.none", pdfpagesof(&a, "%PDF-1.4\n<< /Type /Catalog >>"), "0");
    expect(t, "pdf.cut", pdfpagesof(&a, "%PDF-1.4\n<< /Type /Pages /Count 3"), "0");
    expect(t, "pdf.not", pdfpagesof(&a, "<< /Type /Pages /Count 3 >>"), "3 not a PDF");

    // Encrypted, by its trailer, and not by a longer name
    expect(t, "pdf.encrypted", pdfpagesof(&a,
           "%PDF-1.4\n<< /Type /Pages /Count 1 >>\n"
           "trailer\n<< /Root 1 0 R /Encrypt 9 0 R >>"), "1 encrypted");
    expect(t, "pdf.encrypt name", pdfpagesof(&a,
           "%PDF-1.4\n<< /Type /Pages /Count 1 /EncryptMetadata false >>"), "1");

    // In an object stream, as PDF 1.5 keeps them
    Str inner = "3 0 << /Type /Pages /Kids [] /Count 7 >>";
    Str objs  = concat(&a, "%PDF-1.5\n", pdfobjstm(&a, inner));
    expect(t, "pdf.objstm", pdfpagesof(&a, objs), "7");
    Str parms = concat(&a, "%PDF-1.5\n",
                       pdfobjstm(&a, inner, "/DecodeParms << /Predictor 12 >>"));
    expect(t, "pdf.objstm predictor", pdfpagesof(&a, parms), "0");

    // One that inflates far past its size, past the first guess
    Buf big(&a, 1<<18);
    for (i32 i = 0; i < 100000; i++) {
        putbyte(&big, ' ');
    }
    print(&big, "<< /Type /Pages /Count 31 >>");
    Str huge = concat(&a, "%PDF-1.5\n", pdfobjstm(&a, finish(&big)));
    expect(t, "pdf.objstm grows", pdfpagesof(&a, huge), "31");

    // A damaged stream counts nothing, and reads nothing past its end
    Str bad = "%PDF-1.5\n<< /Type /ObjStm /Filter /FlateDecode >>\nstream\n"
              "\x78\x01\xff\xff\xff\xff\xff\xff" "endstream";
    expect(t, "pdf.objstm damaged", pdfpagesof(&a, bad), "0");
    Str open = "%PDF-1.5\n<< /Type /ObjStm /Filter /FlateDecode >>\nstream\n\x78\x01\x03";
    expect(t, "pdf.objstm open", pdfpagesof(&a, open), "0");
}

// Every object in src, parsed by the reader from a scratch of cap bytes,
// printed canonically and separated by spaces, then why it stopped.
static Str pdfround(Arena *a, Str src, iz cap = 1<<16, b32 stable = 1)
{
    Arena  s = carve(a, cap);
    Pdf    p = pdfstart(s, src, {});
    PdfLex x = {&p, &p.keep, src, 0, stable, 0};
    Buf    b(a, 256);
    for (i32 n = 0; !p.why.data && pdfpeek(&x)>=0; n++) {
        if (n) putbyte(&b, ' ');
        pdfprintobj(&b, pdfparse(&x));
    }
    if (p.why.data) {
        print(&b, " ! ");
        print(&b, p.why);
    }
    return finish(&b);
}

static Str pdfnested(Arena *a, i32 depth, Str open, Str inner, Str close)
{
    Buf b(a, depth*(open.len+close.len) + inner.len);
    for (i32 i = 0; i < depth; i++) print(&b, open);
    print(&b, inner);
    for (i32 i = 0; i < depth; i++) print(&b, close);
    return finish(&b);
}

static b32 pdfless(i32 *a, i32 *b)
{
    return *a < *b;
}

static void test_pdfreader(Test *t)
{
    Arena a = t->perm;

    // Numbers, leniently, as readers take them
    expect(t, "pdf.num", pdfround(&a,
           "1 -2 +3 --5 .5 -.5 4. 0.25 1e3 12abc - . +. 007 -0 3.14159265"),
           "1 -2 3 -5 0.5 -0.5 4 0.25 1000 12 0 0 0 7 0 3.141593");
    expect(t, "pdf.num range", pdfround(&a,
           "9223372036854775807 -9223372036854775808 9223372036854775808 "
           "1.5e400 -1e-400 -0.0000001 123456789012345.5 9999999999.9999 "
           "-99999999.9999999 12345678901.234567"),
           "9223372036854775807 -9223372036854775808 9.223372e18 "
           "1e300 0 0 1.234568e14 9999999999.9999 -100000000 12345678901.235");

    // Names, #xx decoded where it is two hex digits
    expect(t, "pdf.name", pdfround(&a, "/Type /A#20B /#41 /a#2 /a#zz / /x#00y/z"),
           "/Type /A#20B /A /a#232 /a#23zz / /x#00y /z");

    // Literal strings: escapes, continuations, CR folded, nesting
    expect(t, "pdf.str", pdfround(&a,
           "(a\\nb) (\\(x\\)) (\\101\\60\\0601) (line\\\r\ncont) (lf\\\nx) "
           "(raw\r\nx\ry) (\\q) (nest (ed) ok) (\\777) (\\"),
           "(a\\nb) (\\(x\\)) (A001) (linecont) (lfx) (raw\\nx\\ny) (q) "
           "(nest \\(ed\\) ok) (\\377) ()");
    expect(t, "pdf.str open", pdfround(&a, "(abc (d)"), "(abc \\(d\\))");
    expect(t, "pdf.str bytes", pdfround(&a, "(\x01\xff\t)"), "(\\001\\377\\t)");

    // Hex strings, with junk skipped and an odd digit padded
    expect(t, "pdf.hex", pdfround(&a, "<48656c6c6f> <4 8 6> <zz41> <7> <> <41"),
           "(Hello) (H`) (A) (p) () (A)");

    // Containers and references; stray tokens are null
    expect(t, "pdf.dict", pdfround(&a,
           "<< /Type /Page /Parent 2 0 R /Kids [1 0 R 2 0 R] /N 1 2 /K >>"),
           "<</Type /Page /Parent 2 0 R /Kids [1 0 R 2 0 R] /N 1 /K null>>");
    expect(t, "pdf.ref", pdfround(&a, "0 0 R 1 70000 R 1 0 obj 3 0 R"),
           "0 0 null 1 70000 null 1 0 null 3 0 R");
    expect(t, "pdf.keys", pdfround(&a, "true false null obj ] >> ) {}"),
           "true false null null null null null null null");
    expect(t, "pdf.dup", pdfround(&a, "<</A 1/A 2>>[]<<>>"), "<</A 1 /A 2>> [] <<>>");
    expect(t, "pdf.open", pdfround(&a, "[1 2 <</A"), "[1 2 <</A null>>]");
    expect(t, "pdf.comments", pdfround(&a, "1 % c\n2%x\r3%"), "1 2 3");

    // A run too long for a keyword or number is one token
    Str run = pdfnested(&a, 70, "", "", "a");
    expect(t, "pdf.run", pdfround(&a, concat(&a, run, " 1")), "null 1");

    // The first key of a dict wins
    {
        Arena  s = carve(&a, 1<<16);
        Str    d = "<< /A 1 /B (two) /A 3 >>";
        Pdf    p = pdfstart(s, d, {});
        PdfLex x = {&p, &p.keep, d, 0, 1, 0};
        PdfObj o = pdfparse(&x);
        Buf    b(&a, 64);
        pdfprintobj(&b, pdfkey(o, "A"));
        pdfprintobj(&b, pdfkey(o, "B"));
        pdfprintobj(&b, pdfkey(o, "C"));
        pdfprintobj(&b, pdfkey(pdfkey(o, "A"), "A"));
        expect(t, "pdf.key", finish(&b), "1(two)nullnull");
    }

    // Nested PDF_DEPTH deep parses whole; deeper is null where it passes
    // the limit, and what follows the skipped part still parses
    expect(t, "pdf.depth", pdfround(&a, pdfnested(&a, 64, "[", "1", "]")),
           pdfnested(&a, 64, "[", "1", "]"));
    expect(t, "pdf.depth cut", pdfround(&a,
           concat(&a, pdfnested(&a, 65, "<</A ", "[1]", ">>"), " 7")),
           concat(&a, pdfnested(&a, 64, "<</A ", "null", ">>"), " 7"));

    // Nesting 100k deep costs a unit per token and constant stack
    {
        // A carve this large would be sampled by the arena checker, and
        // the reader's own carves inside it taken for overlaps
        i32 n  = 100000;
        Str d  = concat(&a, pdfnested(&a, n, "[", "", "]"), " 5");
        b32 ok = 0;
        i64 w  = 0;
        {
            Pdf    p = pdfstart(a, d, {});
            PdfLex x = {&p, &p.keep, d, 0, 1, 0};
            w = p.work;
            PdfObj o = pdfparse(&x);
            PdfObj f = pdfparse(&x);
            ok = o.kind==PDFO_ARRAY && f.i==5 && !p.why.data;
            w -= p.work;
        }
        expect(t, "pdf.deep", istr(&a, ok), "1");
        expect(t, "pdf.deep work", istr(&a, w), istr(&a, 2*n+1));
    }

    // Tokens from an unstable source are copied out of it
    {
        Arena  s = carve(&a, 1<<16);
        Str    d = clone(&a, "/Name (str) <41>");
        Pdf    p = pdfstart(s, d, {});
        PdfLex x = {&p, &p.keep, d, 0, 0, 0};
        b32    ok = 1;
        for (PdfTok k; (k = pdflex(&x)).kind;) {
            ok &= k.s.data<d.data || k.s.data>=d.data+d.len;
        }
        expect(t, "pdf.unstable", istr(&a, ok), "1");
        expect(t, "pdf.unstable round", pdfround(&a, d, 1<<16, 0), "/Name (str) (A)");
    }

    // Charged allocation fails softly in a tiny arena, and work runs out
    expect(t, "pdf.oom string", pdfround(&a,
           concat(&a, concat(&a, "1 (", pdfnested(&a, 4000, "", "", "\\n")), ") 2"), 4096),
           "1 () ! out of memory");
    Str many = pdfround(&a, pdfnested(&a, 2000, "", "[", "0 "));
    expect(t, "pdf.oom array", istr(&a, startswith(many, "[0 0 ") &&
                                       endswith(many, " 0] ! out of memory")), "1");
    {
        Arena  s = carve(&a, 1<<16);
        Str    d = "1 2 3 4 5 6 7 8 9 10 11 12";
        Pdf    p = pdfstart(s, d, {.work=5});
        PdfLex x = {&p, &p.keep, d, 0, 1, 0};
        i32    n = 0;
        for (; pdflex(&x).kind; n++) {}
        expect(t, "pdf.work", concat(&a, istr(&a, n), concat(&a, " ", p.why)),
               "5 it needs more work than is allowed");
    }
    {
        Arena s = carve(&a, 4096);
        Pdf   p = pdfstart(s, "", {});
        Buf   b(&p.op, 16);
        b32   r1 = pdfreserve(&p, &b, 100);
        b32   r2 = pdfreserve(&p, &b, 1<<20);
        expect(t, "pdf.reserve", concat(&a, istr(&a, r1*10 + r2), p.why), "10out of memory");

        Pdf  q = pdfstart(s, "", {});
        i32 *v = pdfnew<i32>(&q, &q.page, 100);
        for (i32 i = 0; i < 100; i++) v[i] = 100 - i;
        b32  ok = pdfsort(&q, &q.op, v, 100, pdfless);
        b32  no = pdfsort(&q, &q.op, v, 1000, pdfless);
        expect(t, "pdf.sort", concat(&a, istr(&a, ok*1000 + no*100 + v[0]), q.why),
               "1001out of memory");
        expect(t, "pdf.new", istr(&a, !pdfnew<i32>(&q, &q.page, 1)), "1");

        Pdf   m = pdfstart(s, "", {});
        Arena k = pdfmark(&m);
        byte *b0 = m.op.beg;
        pdfnew<u8>(&m, &m.op, 100);
        pdfrewind(&m, &k);
        expect(t, "pdf.rewind", istr(&a, m.op.beg==b0), "1");
    }
}


// Filters: encoders for each, as writers make them, decoded back

struct PdfBits {
    Buf *b;
    u32  acc;
    i32  n;
};

static void pdfemit(PdfBits *w, i32 code, i32 width)
{
    w->acc = w->acc<<width | (u32)code;
    for (w->n += width; w->n >= 8;) {
        w->n -= 8;
        putbyte(w->b, (u8)(w->acc >> w->n));
    }
}

// LZW as PDF's LZWDecode reads it, ported from tests/pdfcorpus.py: a
// clear code first, the width growing one code early unless early is 0.
// Unless clear, a full table stays at 12 bits and takes no entries.
static Str pdflzwenc(Arena *a, Str d, i32 early, b32 clear = 1)
{
    Buf     b(a, d.len*2 + 16);  // 12-bit codes never fill it
    Arena   s     = *a;
    i32    *tab   = alloc<i32>(&s, 4096*256);  // (code, byte) to code
    PdfBits w     = {&b, 0, 0};
    i32     next  = 258;
    i32     width = 9;
    i32     cur   = -1;
    pdfemit(&w, 256, width);
    for (iz i = 0; i < d.len; i++) {
        u8 c = d[i];
        if (cur < 0) {
            cur = c;
            continue;
        } else if (tab[cur*256+c]) {
            cur = tab[cur*256+c];
            continue;
        }
        pdfemit(&w, cur, width);
        if (next < 4096) {
            tab[cur*256+c] = next++;
            if (next+early > 1<<width) {
                if (width < 12) {
                    width++;
                } else if (clear) {
                    pdfemit(&w, 256, width);
                    __builtin_memset(tab, 0, 4096*256*sizeof(*tab));
                    next  = 258;
                    width = 9;
                }
            }
        }
        cur = c;
    }
    if (cur >= 0) {
        pdfemit(&w, cur, width);
    }
    pdfemit(&w, 257, width);
    if (w.n) {
        putbyte(&b, (u8)(w.acc << (8 - w.n)));
    }
    return finish(&b);
}

static Str pdfahxenc(Arena *a, Str d)
{
    Buf b(a, d.len*2 + 1);
    for (iz i = 0; i < d.len; i++) {
        printhex(&b, d[i], 2);
    }
    putbyte(&b, '>');
    return finish(&b);
}

static Str pdfa85enc(Arena *a, Str d)
{
    Buf b(a, d.len*5/4 + 8);
    for (iz i = 0; i < d.len; i += 4) {
        iz  n = d.len-i<4 ? d.len-i : 4;
        u32 v = 0;
        for (iz k = 0; k < 4; k++) {
            v = v<<8 | (k<n ? d[i+k] : 0);
        }
        if (n==4 && !v) {
            putbyte(&b, 'z');
            continue;
        }
        u8 g[5];
        for (i32 k = 4; k >= 0; k--, v /= 85) {
            g[k] = (u8)('!' + v%85);
        }
        print(&b, Str(g, n+1));
    }
    print(&b, "~>");
    return finish(&b);
}

// RunLength, ported from tests/pdfcorpus.py.
static Str pdfrlenc(Arena *a, Str d)
{
    Buf b(a, d.len*2 + 1);
    for (iz i = 0; i < d.len;) {
        iz j = i;
        for (; j<d.len && j-i<128 && d[j]==d[i]; j++) {}
        if (j-i >= 2) {
            putbyte(&b, (u8)(257 - (j - i)));
            putbyte(&b, d[i]);
            i = j;
            continue;
        }
        j = i;
        for (; j<d.len && j-i<128 && !(j+1<d.len && d[j]==d[j+1]); j++) {}
        j = j>i ? j : i+1;
        putbyte(&b, (u8)(j - i - 1));
        print(&b, Str(d.data+i, j-i));
        i = j;
    }
    putbyte(&b, 128);
    return finish(&b);
}

// Rows of rb bytes, each PNG-filtered by the type types[row%ntypes].
static Str pdfpngenc(Arena *a, Str d, iz rb, iz bpp, i32 *types, i32 ntypes)
{
    Buf b(a, d.len + d.len/rb + 1);
    for (iz r = 0; r*rb < d.len; r++) {
        i32 type = types[r % ntypes];
        u8 *row  = d.data + r*rb;
        u8 *up   = r ? row-rb : 0;
        putbyte(&b, (u8)type);
        for (iz j = 0; j<rb && r*rb+j<d.len; j++) {
            i32 x  = j>=bpp ? row[j-bpp] : 0;
            i32 y  = up ? up[j] : 0;
            i32 z  = up && j>=bpp ? up[j-bpp] : 0;
            i32 pr = 0;
            switch (type) {
            case 1: pr = x;                  break;
            case 2: pr = y;                  break;
            case 3: pr = (x + y) / 2;        break;
            case 4: pr = pdfpaeth(x, y, z);  break;
            }
            putbyte(&b, (u8)(row[j] - pr));
        }
    }
    return finish(&b);
}

// TIFF predictor 2 over rows of rb bytes of 8-bit samples.
static Str pdftiffenc(Arena *a, Str d, iz rb, iz colors)
{
    Str r = clone(a, d);
    for (iz at = 0; at < r.len; at += rb) {
        iz n = r.len-at<rb ? r.len-at : rb;
        for (iz j = n-1; j >= colors; j--) {
            r.data[at+j] = (u8)(r[at+j] - r[at+j-colors]);
        }
    }
    return r;
}

// Bytes something like a page's: words, runs, and noise.
static Str pdfsample(Arena *a, iz n)
{
    static Str const words[] = {"BT ", "/F1 12 Tf ", "(Hello) Tj ", "ET\n", "0 0 1 rg "};
    Buf b(a, n + 64);
    u64 s = 1;
    while (b.len < n) {
        s = s*0x5851f42d4c957f2d + 1;
        u32 r = (u32)(s >> 33);
        switch (r % 4) {
        case 0: print(&b, words[(r>>8) % countof(words)]); break;
        case 1: for (u32 k = 0; k < (r>>8)%40; k++) putbyte(&b, (u8)(r>>16)); break;
        default: putbyte(&b, (u8)(r >> 8)); putbyte(&b, (u8)(r >> 20));
        }
    }
    b.len = n;
    return finish(&b);
}

struct PdfDecoded {
    Str data;
    Str why;
    i32 image;
    i32 damage;
    b32 cut;
    Str parms;  // printed
};

// Decoded per the stream dict in text, from raw, at most cap bytes, in a
// reader over a scratch of room bytes, or all of a, the data copied out
// into a. (A large carve would be sampled by the arena checker, and the
// reader's own carves inside it taken for overlaps.)
static PdfDecoded pdfdec(Arena *a, Str dict, Str raw, iz cap = 0,
                         PdfLimits lim = {}, iz room = 0)
{
    PdfDecoded r = {};
    Arena      s = room ? carve(a, room) : *a;
    Pdf        p = pdfstart(s, raw, lim);
    PdfLex     x = {&p, &p.keep, dict, 0, 1, 0};
    PdfObj     d = pdfparse(&x);
    PdfData    o = pdfdecode(&p, &p.page, d, raw, cap);
    r.data   = clone(a, o.data);
    r.why    = p.why;
    r.image  = o.image;
    r.damage = p.damage;
    r.cut    = o.cut;
    Buf b(a, 64);
    pdfprintobj(&b, o.parms);
    r.parms = finish(&b);
    return r;
}

// The flags of a decode, short: c(ut), d(amage)N, i(mage)N, then why.
static Str pdfflags(Arena *a, PdfDecoded d)
{
    Buf b(a, 64);
    if (d.cut)    putbyte(&b, 'c');
    if (d.damage) { putbyte(&b, 'd'); print(&b, (i64)d.damage); }
    if (d.image)  { putbyte(&b, 'i'); print(&b, (i64)d.image); }
    if (d.why.data) { putbyte(&b, ' '); print(&b, d.why); }
    return finish(&b);
}

// One filter on in through pdffilter, at most cap bytes.
static Str pdfone(Arena *a, i32 id, Str parms, Str in, iz cap)
{
    Pdf     p = pdfstart(*a, in, {});
    PdfLex  x = {&p, &p.keep, parms, 0, 1, 0};
    PdfObj  d = pdfparse(&x);
    PdfData o = pdffilter(&p, &p.page, id, d, in, cap);
    return clone(a, o.data);
}

static void test_pdffilters(Test *t)
{
    Arena a    = t->perm;
    Str   data = pdfsample(&a, 30000);

    // Round trips, through every stage of LZW's table, cleared or full
    {
        Str l1 = pdflzwenc(&a, data, 1);
        Str l0 = pdflzwenc(&a, data, 0);
        Str ln = pdflzwenc(&a, data, 1, 0);
        expect(t, "pdf.lzw", pdfdec(&a, "<</Filter/LZWDecode>>", l1).data, data);
        expect(t, "pdf.lzw early0", pdfdec(&a,
               "<</Filter/LZWDecode/DecodeParms<</EarlyChange 0>>>>", l0).data, data);
        expect(t, "pdf.lzw full", pdfdec(&a, "<</Filter/LZWDecode>>", ln).data, data);
        expect(t, "pdf.lzw wrong", istr(&a, pdfdec(&a,
               "<</Filter/LZWDecode/DecodeParms<</EarlyChange 0>>>>", l1).data==data), "0");
        Str kwk = "abababababababababaaaaaaaaaaaaaaaaaaaaaa";
        expect(t, "pdf.lzw kwkwk", pdfdec(&a, "<</Filter/LZW>>", pdflzwenc(&a, kwk, 1)).data, kwk);
        expect(t, "pdf.lzw empty", pdfflags(&a, pdfdec(&a, "<</Filter/LZW>>", pdflzwenc(&a, "", 1))), "");
    }
    expect(t, "pdf.ahx", pdfdec(&a, "<</Filter/ASCIIHexDecode>>", pdfahxenc(&a, data)).data, data);
    expect(t, "pdf.a85", pdfdec(&a, "<</Filter/ASCII85Decode>>", pdfa85enc(&a, data)).data, data);
    expect(t, "pdf.rl", pdfdec(&a, "<</Filter/RunLengthDecode>>", pdfrlenc(&a, data)).data, data);
    {
        iz  cap = data.len*9/8 + 16;
        u8 *out = alloc<u8>(&a, cap);
        Str raw = Str(out, zdeflate(a, out, cap, data.data, data.len));
        Str zl  = concat(&a, concat(&a, "\x78\x01", raw), "\0\0\0\0");
        expect(t, "pdf.flate", pdfdec(&a, "<</Filter/FlateDecode>>", zl).data, data);
        expect(t, "pdf.flate raw", pdfdec(&a, "<</Filter/FlateDecode>>", raw).data, data);

        // Damaged or cut short, it keeps what came before the fault
        PdfDecoded h = pdfdec(&a, "<</Filter/Fl>>", takehead(zl, zl.len/2));
        expect(t, "pdf.flate half", istr(&a, h.data.len>data.len/4 &&
               startswith(data, h.data)), "1");
        expect(t, "pdf.flate half flags", pdfflags(&a, h), "d1");
        expect(t, "pdf.flate junk", pdfflags(&a, pdfdec(&a, "<</Filter/Fl>>", "\xff\xff\xff")), "d1");
        expect(t, "pdf.flate none", pdfflags(&a, pdfdec(&a, "<</Filter/Fl>>", "")), "d1");

        // At the caller's cap, or the decoded budget's, a prefix is kept
        PdfDecoded c = pdfdec(&a, "<</Filter/Fl>>", zl, 1000);
        expect(t, "pdf.flate cap", c.data, takehead(data, 1000));
        expect(t, "pdf.flate cap flags", pdfflags(&a, c), "c");
        PdfDecoded b = pdfdec(&a, "<</Filter/Fl>>", zl, 0, {.decoded=5000});
        expect(t, "pdf.flate budget", istr(&a, b.data.len<=5000 && startswith(data, b.data)), "1");
        expect(t, "pdf.flate budget flags", pdfflags(&a, b), "c it decodes to more than is allowed");

        // Into a small arena, the output stops at half its room
        {
            Arena   s = a;
            Pdf     p = pdfstart(s, zl, {});
            Arena   m = carve(&p.page, 1<<14);
            PdfLex  x = {&p, &p.keep, "<</Filter/Fl>>", 0, 1, 0};
            PdfData o = pdfdecode(&p, &m, pdfparse(&x), zl, 0);
            expect(t, "pdf.flate memory", istr(&a, o.data.len<=(1<<13) && o.cut &&
                   startswith(data, o.data)), "1");
            expect(t, "pdf.flate memory why", p.why, "out of memory");
        }
    }

    // A bomb, past the first guess and then the cap
    {
        Str zeros = Str(alloc<u8>(&a, 1<<20), 1<<20);
        iz  cap   = zeros.len*9/8 + 16;
        u8 *out   = alloc<u8>(&a, cap);
        Str z     = Str(out, zdeflate(a, out, cap, zeros.data, zeros.len));
        expect(t, "pdf.flate grows", pdfdec(&a, "<</Filter/Fl>>", z).data, zeros);
        PdfDecoded c = pdfdec(&a, "<</Filter/Fl>>", z, 300000);
        expect(t, "pdf.flate grows cap", concat(&a, istr(&a, c.data.len), pdfflags(&a, c)), "300000c");

        // The budget left after the first guess is less than it gave:
        // what that gave is kept
        Str small = takehead(zeros, 1<<16);
        Str zs    = Str(out, zdeflate(a, out, cap, small.data, small.len));
        PdfDecoded k = pdfdec(&a, "<</Filter/Fl>>", zs, 0, {.decoded=6000});
        expect(t, "pdf.flate keep", concat(&a, istr(&a, k.data.len), pdfflags(&a, k)),
               "4096c it decodes to more than is allowed");
        expect(t, "pdf.flate keep data", k.data, takehead(zeros, 4096));
    }


    // Chains, with parameters aligned, nulls among them, and abbreviated
    {
        Str chain = pdfahxenc(&a, pdflzwenc(&a, data, 0));
        expect(t, "pdf.chain", pdfdec(&a,
               "<</Filter[/ASCIIHexDecode/LZWDecode]/DecodeParms[null<</EarlyChange 0>>]>>",
               chain).data, data);
        expect(t, "pdf.chain inline", pdfdec(&a, "<</F[/AHx/LZW]/DP[null<</EarlyChange 0>>]>>",
               chain).data, data);
        // Stopped by the budget before the last stage: no data, as the
        // first stage's output is not the stream's
        PdfDecoded e = pdfdec(&a, "<</Filter[/LZW/AHx]>>", pdflzwenc(&a, pdfahxenc(&a, data), 1),
                              0, {.decoded=30000});
        expect(t, "pdf.chain stopped", concat(&a, istr(&a, e.data.len), pdfflags(&a, e)),
               "0c it decodes to more than is allowed");
        Str three = pdfa85enc(&a, pdfrlenc(&a, pdfahxenc(&a, data)));
        expect(t, "pdf.chain three", pdfdec(&a, "<</Filter[/A85/RL/AHx]>>", three).data, data);
        PdfDecoded c = pdfdec(&a, "<</Filter[/A85/RL/AHx]>>", three, 77);
        expect(t, "pdf.chain cap", c.data, takehead(data, 77));
        expect(t, "pdf.chain cap flags", pdfflags(&a, c), "c");
        Str crypt = pdfdec(&a, "<</Filter[/Crypt/AHx]/DecodeParms[<</Name/Identity>>null]>>",
                           "414243>").data;
        expect(t, "pdf.crypt", crypt, "ABC");
        expect(t, "pdf.crypt other", pdfflags(&a, pdfdec(&a,
               "<</Filter/Crypt/DecodeParms<</Name/StdCF>>>>", "abc")), "d1");
    }

    // Unfiltered, unknown and image filters
    {
        expect(t, "pdf.plain", pdfdec(&a, "<<>>", "abc").data, "abc");
        PdfDecoded c = pdfdec(&a, "<<>>", "abcdef", 4);
        expect(t, "pdf.plain cap", concat(&a, c.data, pdfflags(&a, c)), "abcdc");
        PdfDecoded u = pdfdec(&a, "<</Filter[/AHx/Foo]>>", "41>");
        expect(t, "pdf.unknown", concat(&a, u.data, pdfflags(&a, u)), "d1");
        expect(t, "pdf.notname", pdfflags(&a, pdfdec(&a, "<</Filter 5>>", "41>")), "d1");
        expect(t, "pdf.toomany", pdfflags(&a, pdfdec(&a,
               "<</Filter[/AHx/AHx/AHx/AHx/AHx/AHx/AHx/AHx/AHx]>>", "41>")), "d1");
        PdfDecoded d = pdfdec(&a, "<</Filter[/AHx/DCT]/DecodeParms[null<</ColorTransform 0>>]>>",
                              "FFD8>");
        expect(t, "pdf.dct", concat(&a, d.data, pdfflags(&a, d)), "\xff\xd8i1");
        expect(t, "pdf.dct parms", d.parms, "<</ColorTransform 0>>");
        PdfDecoded g = pdfdec(&a, "<</F/CCF/DP<</K -1/Columns 8>>>>", "xy");
        expect(t, "pdf.ccitt", concat(&a, g.data, pdfflags(&a, g)), "xyi2");
        expect(t, "pdf.ccitt parms", g.parms, "<</K -1 /Columns 8>>");
        expect(t, "pdf.jpx", pdfflags(&a, pdfdec(&a, "<</Filter/JPXDecode>>", "x")), "i3");
        expect(t, "pdf.jbig2", pdfflags(&a, pdfdec(&a, "<</Filter[/JBIG2Decode/AHx]>>", "x")), "d1i4");
    }

    // Damaged and odd inputs to each decoder
    expect(t, "pdf.ahx odd", pdfdec(&a, "<</Filter/AHx>>", " 4 1\n4>").data, "A@");
    expect(t, "pdf.ahx open", pdfdec(&a, "<</Filter/AHx>>", "414").data, "A");
    PdfDecoded hj = pdfdec(&a, "<</Filter/AHx>>", "41x42>");
    expect(t, "pdf.ahx junk", concat(&a, hj.data, pdfflags(&a, hj)), "Ad1");
    expect(t, "pdf.a85 frame", pdfdec(&a, "<</Filter/A85>>", " <~9jqo^~>").data, "Man ");
    expect(t, "pdf.a85 partial", pdfdec(&a, "<</Filter/A85>>", "9jqo^BlbD~>").data, "Man is ");
    expect(t, "pdf.a85 open", pdfdec(&a, "<</Filter/A85>>", "9jqo^BlbD").data, "Man ");
    expect(t, "pdf.a85 z", pdfdec(&a, "<</Filter/A85>>", "z9jqo^z~>").data,
           "\0\0\0\0Man \0\0\0\0");
    PdfDecoded az = pdfdec(&a, "<</Filter/A85>>", "9jzqo^~>");
    expect(t, "pdf.a85 z inside", concat(&a, az.data, pdfflags(&a, az)), "d1");
    PdfDecoded ao = pdfdec(&a, "<</Filter/A85>>", "9jqo^uuuuu~>");
    expect(t, "pdf.a85 over", concat(&a, ao.data, pdfflags(&a, ao)), "Man d1");
    expect(t, "pdf.a85 one", pdfflags(&a, pdfdec(&a, "<</Filter/A85>>", "9jqo^B~>")), "d1");
    expect(t, "pdf.a85 junk", pdfflags(&a, pdfdec(&a, "<</Filter/A85>>", "9jqo^\x7f~>")), "d1");
    PdfDecoded ap = pdfdec(&a, "<</Filter/A85>>", "9jqo^uuu~>");
    expect(t, "pdf.a85 partial over", concat(&a, ap.data, pdfflags(&a, ap)), "Man d1");
    PdfDecoded dc = pdfdec(&a, "<</Filter/DCT>>", "\xff\xd8\xff\xe0", 2);
    expect(t, "pdf.dct cap", concat(&a, dc.data, pdfflags(&a, dc)), "\xff\xd8" "ci1");
    expect(t, "pdf.rl short", pdfdec(&a, "<</Filter/RL>>", "\x05" "abc").data, "abc");
    expect(t, "pdf.rl repeat", pdfdec(&a, "<</Filter/RL>>", "\xfe" "x" "\x80" "yy").data, "xxx");
    expect(t, "pdf.rl lone", pdfdec(&a, "<</Filter/RL>>", "\x00" "a" "\xfe").data, "a");
    {
        // A code past the next to be defined stops, keeping what came before
        Buf     b(&a, 16);
        PdfBits w = {&b, 0, 0};
        pdfemit(&w, 'A', 9);
        pdfemit(&w, 'B', 9);
        pdfemit(&w, 300, 9);
        pdfemit(&w, 'C', 9);
        pdfemit(&w, 0, 7);
        PdfDecoded d = pdfdec(&a, "<</Filter/LZW>>", finish(&b));
        expect(t, "pdf.lzw bad", concat(&a, d.data, pdfflags(&a, d)), "ABd1");
    }

    // A prefix of the input decodes to a prefix of the output
    {
        Str ins[] = {
            pdflzwenc(&a, data, 1), pdflzwenc(&a, data, 0), pdfahxenc(&a, data),
            pdfa85enc(&a, data),    pdfrlenc(&a, data),
        };
        i32 ids[]   = {PDFF_LZW, PDFF_LZW, PDFF_AHX, PDFF_A85, PDFF_RL};
        Str parms[] = {"<<>>", "<</EarlyChange 0>>", "<<>>", "<<>>", "<<>>"};
        b32 ok      = 1;
        for (i32 i = 0; i < countof(ins); i++) {
            Str full = pdfone(&a, ids[i], parms[i], ins[i], 1<<20);
            ok &= full == data;
            for (iz n = 0; n < ins[i].len; n += 1 + n/3) {
                Arena s = a;
                ok &= startswith(full, pdfone(&s, ids[i], parms[i], takehead(ins[i], n), 1<<20));
            }
        }
        expect(t, "pdf.prefix", istr(&a, ok), "1");
    }

    // Predictors: PNG with every row type, TIFF, and their parameters
    {
        Str img   = takehead(data, 15*40 + 7);  // a partial last row
        i32 all[] = {0, 1, 2, 3, 4};
        Str png   = pdfpngenc(&a, img, 15, 3, all, countof(all));
        Str zl    = {};
        {
            iz  cap = png.len*9/8 + 16;
            u8 *out = alloc<u8>(&a, cap);
            zl = concat(&a, "\x78\x01", Str(out, zdeflate(a, out, cap, png.data, png.len)));
        }
        expect(t, "pdf.png", pdfdec(&a, "<</Filter/FlateDecode/DecodeParms"
               "<</Predictor 15/Colors 3/Columns 5>>>>", zl).data, img);
        expect(t, "pdf.png lzw", pdfdec(&a, "<</Filter/LZWDecode/DecodeParms"
               "<</Predictor 12/Colors 3/Columns 5>>>>", pdflzwenc(&a, png, 1)).data, img);
        i32 one[] = {4};
        Str bits  = pdfpngenc(&a, takehead(data, 60), 3, 1, one, 1);
        expect(t, "pdf.png bits", pdfdec(&a, "<</Filter/LZW/DecodeParms"
               "<</Predictor 10/Colors 3/BitsPerComponent 1/Columns 8>>>>",
               pdflzwenc(&a, bits, 1)).data, takehead(data, 60));
        expect(t, "pdf.png unknown", pdfdec(&a, "<</Filter/LZW/DecodeParms"
               "<</Predictor 12/Columns 2>>>>", pdflzwenc(&a, "\x07" "AB\x02\x01\x01\x03", 1)).data,
               "ABBC");
        Str prefix = "pdf.png prefix";
        b32 ok     = 1;
        Str full   = pdfone(&a, PDFF_LZW, "<</Predictor 15/Colors 3/Columns 5>>",
                            pdflzwenc(&a, png, 1), 1<<20);
        for (iz n = 0; n <= png.len; n++) {
            Arena s = a;
            ok &= startswith(full, pdfone(&s, PDFF_LZW, "<</Predictor 15/Colors 3/Columns 5>>",
                                         pdflzwenc(&s, takehead(png, n), 1), 1<<20));
        }
        expect(t, prefix, istr(&a, ok && full==img), "1");

        Str tiff = pdftiffenc(&a, img, 15, 3);
        expect(t, "pdf.tiff", pdfdec(&a, "<</Filter/LZW/DecodeParms"
               "<</Predictor 2/Colors 3/Columns 5>>>>", pdflzwenc(&a, tiff, 1)).data, img);
        // 16-bit samples carry into their high byte; a lone half is dropped
        expect(t, "pdf.tiff 16", pdfdec(&a, "<</Filter/LZW/DecodeParms"
               "<</Predictor 2/BitsPerComponent 16/Columns 3>>>>",
               pdflzwenc(&a, "\x00\xff\x00\x01\xff\xff\x01", 1)).data,
               "\x00\xff\x01\x00\x00\xff");
        // 2-bit samples wrap within themselves: 1 2 3 0 becomes 1 3 2 2
        expect(t, "pdf.tiff 2", pdfdec(&a, "<</Filter/LZW/DecodeParms"
               "<</Predictor 2/BitsPerComponent 2/Columns 4>>>>",
               pdflzwenc(&a, "\x6c", 1)).data, "\x7a");
        expect(t, "pdf.tiff 2 rows", pdfdec(&a, "<</Filter/LZW/DecodeParms"
               "<</Predictor 2/BitsPerComponent 2/Columns 3>>>>",
               pdflzwenc(&a, "\x6c\x6c", 1)).data, "\x78\x78");
        expect(t, "pdf.tiff ahx", pdfdec(&a, "<</Filter/AHx/DecodeParms"
               "<</Predictor 2/BitsPerComponent 2/Columns 4>>>>", "6C>").data, "\x6c");

        Str bad[] = {
            "<</Predictor 3>>", "<</Predictor 12/Colors 0>>", "<</Predictor 12/Colors 33>>",
            "<</Predictor 2/BitsPerComponent 3>>", "<</Predictor 12/Columns 0>>",
            "<</Predictor 12/Columns 16777217>>",
        };
        b32 all0 = 1;
        for (i32 i = 0; i < countof(bad); i++) {
            Str d = concat(&a, concat(&a, "<</Filter/LZW/DecodeParms", bad[i]), ">>");
            PdfDecoded r = pdfdec(&a, d, pdflzwenc(&a, "abc", 1));
            all0 &= !r.data.len && r.damage==1;
        }
        expect(t, "pdf.predictor bad", istr(&a, all0), "1");
        expect(t, "pdf.predictor none", pdfdec(&a, "<</Filter/AHx/DecodeParms"
               "<</Predictor 15>>>>", "4142>").data, "AB");
        expect(t, "pdf.predictor 1", pdfdec(&a, "<</Filter/LZW/DecodeParms"
               "<</Predictor 1/Columns 0>>>>", pdflzwenc(&a, "abc", 1)).data, "abc");
    }
}


// The document layer: files written by pdfbuild, a port of the corpus
// writer (tests/pdfcorpus.py's Doc) with its damage cases as flags,
// loaded, resolved and walked; then the same files cut, corrupted, and
// read in too little memory or budget.

enum : i32 {
    PDFB_SHIFT    = 1 << 0,  // ten bytes after the header: offsets off
    PDFB_XREFNONE = 1 << 1,  // cut at the xref: no xref, trailer, startxref
    PDFB_SX0      = 1 << 2,  // startxref 0
    PDFB_SXPAST   = 1 << 3,  // startxref past the end
    PDFB_LF19     = 1 << 4,  // xref rows of 19 bytes, a bare LF
    PDFB_JUNK     = 1 << 5,  // junk before %PDF-, not counted by offsets
    PDFB_PREVLOOP = 1 << 6,  // /Prev to its own section
    PDFB_HYBRID   = 1 << 7,  // a table, objects in an object stream by /XRefStm
    PDFB_XREFSTM  = 1 << 8,  // an xref stream instead of a table
    PDFB_OBJSTM   = 1 << 9,  // all but streams in an object stream
    PDFB_ENCRYPT  = 1 << 10, // /Encrypt in the trailer or xref stream
    PDFB_INFO     = 1 << 11, // /Info 3 0 R in the trailer (a table's)
};

// A stream object's body, its /Length added.
static Str pdfbstream(Arena *a, Str dict, Str data)
{
    Buf b(a, 64 + dict.len + data.len);
    print(&b, "<< ");
    print(&b, dict);
    print(&b, " /Length ");
    print(&b, data.len);
    print(&b, " >>\nstream\n");
    print(&b, data);
    print(&b, "\nendstream");
    return finish(&b);
}

// A stream's data deflated as zlib, with a trailer no reader checks.
static Str pdfbzlib(Arena *a, Str d)
{
    iz  cap = d.len*9/8 + 64;
    u8 *out = alloc<u8>(a, cap);
    iz  n   = zdeflate(*a, out, cap, d.data, d.len);
    return concat(a, concat(a, "\x78\x01", Str(out, n)), "\0\0\0\0");
}

// An xref stream's body: rows of /W [1 4 2], PNG Up predicted, Flate.
static Str pdfbxrefstm(Arena *a, i32 *types, i64 *f2, i32 *f3, i32 size, Str extra)
{
    u8 *rows = alloc<u8>(a, (iz)size*8);
    u8  prev[7] = {};
    for (i32 i = 0; i < size; i++) {
        u8 row[7] = {(u8)types[i], (u8)(f2[i]>>24), (u8)(f2[i]>>16), (u8)(f2[i]>>8),
                     (u8)f2[i], (u8)(f3[i]>>8), (u8)f3[i]};
        rows[i*8] = 2;
        for (i32 j = 0; j < 7; j++) {
            rows[i*8+1+j] = (u8)(row[j] - prev[j]);
            prev[j] = row[j];
        }
    }
    Buf d(a, 256);
    print(&d, "/Type /XRef /Size ");
    print(&d, (i64)size);
    print(&d, " /Root 1 0 R /W [1 4 2] /Filter /FlateDecode "
              "/DecodeParms << /Predictor 12 /Columns 7 >>");
    print(&d, extra);
    Str dict = finish(&d);
    return pdfbstream(a, dict, pdfbzlib(a, Str(rows, (iz)size*8)));
}

static b32 pdfbisstream(Str body)
{
    return find(body, "\nstream\n") >= 0;
}

// A PDF of objects 1..n from objs, the first its catalog, written as
// the flags say.
static Str pdfbuild(Arena *a, Str *objs, i32 n, i32 flags)
{
    b32 objstm = (flags & PDFB_OBJSTM) || (flags & PDFB_HYBRID);
    b32 hybrid = flags & PDFB_HYBRID;
    b32 xstm   = (flags & PDFB_XREFSTM) || (objstm && !hybrid);
    i32 box    = n + 1;                      // the object stream
    i32 xnum   = objstm ? n+2 : n+1;         // an xref stream
    i32 size   = xstm||hybrid ? xnum+1 : objstm ? box+1 : n+1;
    i32 *types = alloc<i32>(a, size);
    i64 *f2    = alloc<i64>(a, size);
    i32 *f3    = alloc<i32>(a, size);
    types[0] = 0;
    f3[0]    = 0xffff;

    Buf out(a, 1<<12);
    print(&out, "%PDF-1.5\n%\xe2\xe3\xcf\xd3\n");
    for (i32 i = 1; i <= n; i++) {
        Str body = objs[i-1];
        if (objstm && !pdfbisstream(body)) {
            continue;
        }
        types[i] = 1;
        f2[i]    = out.len;
        print(&out, (i64)i);
        print(&out, " 0 obj\n");
        print(&out, body);
        print(&out, "\nendobj\n");
    }
    if (objstm) {
        Buf head(a, 256);
        Buf body(a, 1<<12);
        i32 k = 0;
        for (i32 i = 1; i <= n; i++) {
            if (pdfbisstream(objs[i-1])) continue;
            types[i] = 2;
            f2[i]    = box;
            f3[i]    = k++;
            print(&head, (i64)i);
            putbyte(&head, ' ');
            print(&head, body.len);
            putbyte(&head, ' ');
            print(&body, objs[i-1]);
            putbyte(&body, '\n');
        }
        Buf d(a, 128);
        print(&d, "/Type /ObjStm /N ");
        print(&d, (i64)k);
        print(&d, " /First ");
        print(&d, head.len);
        print(&d, " /Filter /FlateDecode");
        types[box] = 1;
        f2[box]    = out.len;
        print(&out, (i64)box);
        print(&out, " 0 obj\n");
        print(&out, pdfbstream(a, finish(&d), pdfbzlib(a, concat(a, finish(&head), finish(&body)))));
        print(&out, "\nendobj\n");
    }

    // A hybrid's xref stream holds what its table marks free
    iz xs = -1;
    if (hybrid) {
        i32 *t2 = alloc<i32>(a, size);
        for (i32 i = 0; i < size; i++) t2[i] = types[i]==2 ? 2 : 0;
        xs = out.len;
        types[xnum] = 1;
        f2[xnum]    = xs;
        print(&out, (i64)xnum);
        print(&out, " 0 obj\n");
        print(&out, pdfbxrefstm(a, t2, f2, f3, size, ""));
        print(&out, "\nendobj\n");
    }

    iz x = out.len;
    if (xstm) {
        types[xnum] = 1;
        f2[xnum]    = x;
        Buf e(a, 64);
        if (flags & PDFB_PREVLOOP) {
            print(&e, " /Prev ");
            print(&e, x);
        }
        if (flags & PDFB_ENCRYPT) {
            print(&e, " /Encrypt 99 0 R");
        }
        print(&out, (i64)xnum);
        print(&out, " 0 obj\n");
        print(&out, pdfbxrefstm(a, types, f2, f3, size, finish(&e)));
        print(&out, "\nendobj\n");
    } else {
        print(&out, "xref\n0 ");
        print(&out, (i64)size);
        putbyte(&out, '\n');
        for (i32 i = 0; i < size; i++) {
            b32 used = types[i] == 1;
            printpad(&out, used ? f2[i] : 0, 10);
            putbyte(&out, ' ');
            printpad(&out, used ? 0 : 65535, 5);
            print(&out, used ? Str(" n") : " f");
            print(&out, flags&PDFB_LF19 ? Str("\n") : " \n");
        }
        print(&out, "trailer\n<< /Size ");
        print(&out, (i64)size);
        print(&out, " /Root 1 0 R");
        if (xs >= 0) {
            print(&out, " /XRefStm ");
            print(&out, xs);
        }
        if (flags & PDFB_PREVLOOP) {
            print(&out, " /Prev ");
            print(&out, x);
        }
        if (flags & PDFB_ENCRYPT) {
            print(&out, " /Encrypt 99 0 R");
        }
        if (flags & PDFB_INFO) {
            print(&out, " /Info 3 0 R");
        }
        print(&out, " >>\n");
    }
    print(&out, "startxref\n");
    print(&out, flags&PDFB_SX0 ? 0 : flags&PDFB_SXPAST ? 999999999 : (i64)x);
    print(&out, "\n%%EOF\n");

    Str r = finish(&out);
    if (flags & PDFB_XREFNONE) {
        r = takehead(r, x);
    }
    if (flags & PDFB_SHIFT) {
        iz h = find(r, "\n") + 1;
        r = concat(a, concat(a, takehead(r, h), "% shifted\n"), cuthead(r, h));
    }
    if (flags & PDFB_JUNK) {
        Buf j(a, 1024);
        for (i32 i = 0; i < 30; i++) print(&j, "Junk from a mail gateway.\r\n");
        r = concat(a, finish(&j), r);
    }
    return r;
}

// Three pages in a tree of two levels, inheriting a MediaBox, one of
// its own and a Rotate; contents direct, with a /Length that comes
// later in the file, and through a filter; the tree's /Count lies.
static Str *pdfbdoc(Arena *a, i32 *n)
{
    static Str const fixed[] = {
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 99 /MediaBox [0 0 612 792] "
            "/Resources << /Font << /F1 9 0 R >> >> >>",
        "<< /Type /Page /Parent 2 0 R /Contents 6 0 R /Rotate 90 >>",
        "<< /Type /Pages /Parent 2 0 R /Kids [5 0 R 7 0 R] /MediaBox [0 0 100 100] >>",
        "<< /Type /Page /Parent 4 0 R /Contents 8 0 R >>",
        {},
        "<< /Type /Page /Parent 4 0 R /Contents 10 0 R /Rotate -270 >>",
        "<< /Length 11 0 R >>\nstream\npage two\nendstream",
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
        {},
        "8",
    };
    Str *objs = alloc<Str>(a, countof(fixed));
    for (i32 i = 0; i < countof(fixed); i++) objs[i] = fixed[i];
    objs[5] = pdfbstream(a, "", "page one");
    objs[9] = pdfbstream(a, "/Filter /ASCIIHexDecode", "70 61 67 65 20 74 68 72 65 65>");
    *n = countof(fixed);
    return objs;
}

// A read summed up: its status and page count, whether recovered, and
// each page's decoded contents, then why it stopped. The reader runs
// in a copy of a, after the summary's buffer, which does not grow, in
// room bytes if room is set.
static Str pdfsummary(Arena *a, Str pdf, PdfLimits lim = {}, iz room = 0)
{
    Buf   b(a, 1<<12);
    Arena s = *a;
    if (room) {
        s.end = s.beg + room;
    }
    Pdf p = pdfstart(s, pdf, lim);
    i32 r = pdfload(&p);
    print(&b, r==PDFR_OK ? Str("ok") : r==PDFR_NOTPDF ? Str("notpdf")
            : r==PDFR_ENCRYPTED ? Str("encrypted") : "?");
    PdfPages g = r==PDFR_NOTPDF ? PdfPages{} : pdfpages(&p);
    putbyte(&b, ' ');
    print(&b, g.pages.len);
    if (p.recovered) print(&b, " rec");
    if (g.fallback)  print(&b, " fb");
    print(&b, ":");
    for (iz i = 0; i<g.pages.len && b.len<3000; i++) {
        PdfData d = pdfstream(&p, &p.page, pdfget(&p, g.pages.data[i].page, "Contents"), 0);
        print(&b, i ? Str("|") : " ");
        print(&b, takehead(d.data, d.data.len<64 ? d.data.len : 64));
    }
    if (p.why.data) {
        print(&b, " ! ");
        print(&b, p.why);
    }
    assert(b.cap == 1<<12);  // or it grew over the reader
    return finish(&b);
}

// A summary without its note of recovery.
static Str pdfnorec(Arena *a, Str s)
{
    iz at = find(s, " rec");
    return at<0 ? s : concat(a, takehead(s, at), cuthead(s, at+4));
}

// Every prefix of pdf, copied to its own exact size, read without fault;
// from the first past the last object on, read whole: want. Returns the
// first that is not, and how it read, or empty.
static Str pdfcutsweep(Arena *a, Str pdf, iz whole, Str want)
{
    for (iz n = 0; n <= pdf.len; n++) {
        Arena s   = *a;
        u8   *cpy = allocbytes(&s, n);  // so that nothing reads past it
        copybytes(cpy, pdf.data, n);
        Str r = pdfnorec(&s, pdfsummary(&s, {cpy, n}));
        if (n>=whole && !(r==want)) {
            return concat(a, concat(a, istr(a, n), " "), r);
        }
    }
    return {};
}

// Each byte XORed in turn, read without fault: how many read differently,
// recovered or not.
static i32 pdfcorruptsweep(Arena *a, Str pdf, Str want)
{
    i32 changed = 0;
    for (iz i = 0; i < pdf.len; i++) {
        Arena s   = *a;
        u8   *cpy = allocbytes(&s, pdf.len);
        copybytes(cpy, pdf.data, pdf.len);
        cpy[i] ^= (u8)(1 + i%255);
        changed += !(pdfnorec(&s, pdfsummary(&s, {cpy, pdf.len})) == want);
    }
    return changed;
}

// In scratches of fixed sizes, each read stops for memory or reads it
// all; and some are big enough.
static b32 pdfmemsweep(Arena *a, Str pdf, Str want)
{
    b32 ok   = 1;
    b32 some = 0;
    for (iz room = 1024; ok && room <= 1<<22; room *= 2) {
        Str r = pdfsummary(a, pdf, {}, room);
        ok    = r==want || endswith(r, " ! out of memory");
        some |= r == want;
    }
    return ok && some;
}

// With work budgets doubling from 1, each read stops for want of work
// or reads it all; and some budgets are enough.
static b32 pdfworksweep(Arena *a, Str pdf, Str want)
{
    b32 ok   = 1;
    b32 some = 0;
    for (i64 w = 1; ok && w <= 1<<20; w *= 2) {
        Str r = pdfsummary(a, pdf, {.work=w});
        ok    = r==want || endswith(r, " ! it needs more work than is allowed");
        some |= r == want;
    }
    return ok && some;
}

// A file of prefix, a header, and objects 1..n, with a classic xref
// whose offsets count from the start of the file, and extra in its
// trailer.
static Str pdfhand(Arena *a, Str prefix, Str *objs, i32 n, Str extra)
{
    Buf b(a, 1<<14);
    iz *offs = alloc<iz>(a, n+1);
    print(&b, prefix);
    print(&b, "%PDF-1.4\n");
    for (i32 i = 1; i <= n; i++) {
        offs[i] = b.len;
        print(&b, (i64)i);
        print(&b, " 0 obj\n");
        print(&b, objs[i-1]);
        print(&b, "\nendobj\n");
    }
    iz x = b.len;
    print(&b, "xref\n0 ");
    print(&b, (i64)n+1);
    print(&b, "\n0000000000 65535 f \n");
    for (i32 i = 1; i <= n; i++) {
        printpad(&b, offs[i], 10);
        print(&b, " 00000 n \n");
    }
    print(&b, "trailer\n<< /Size ");
    print(&b, (i64)n+1);
    print(&b, " /Root 1 0 R");
    print(&b, extra);
    print(&b, " >>\nstartxref\n");
    print(&b, x);
    print(&b, "\n%%EOF\n");
    assert(b.cap == 1<<14);
    return finish(&b);
}

// An incremental update of base rewriting the catalog, the tree and
// page one's object, which still uses contents only base has, with its
// /Prev off by skew, or prev itself if set.
static Str pdfrevise(Arena *a, Str base, iz skew, Str prev = {})
{
    iz  sx = find(base, "startxref\n") + 10;
    Str p  = cut(cuthead(base, sx), '\n').head;
    Str objs[] = {
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /Contents 6 0 R >>",
    };
    Buf b(a, 1<<12);
    print(&b, base);
    iz offs[3] = {};
    for (i32 i = 0; i < 3; i++) {
        offs[i] = b.len;
        print(&b, (i64)i+1);
        print(&b, " 0 obj\n");
        print(&b, objs[i]);
        print(&b, "\nendobj\n");
    }
    iz x = b.len;
    print(&b, "xref\n0 4\n0000000000 65535 f \n");
    for (i32 i = 0; i < 3; i++) {
        printpad(&b, offs[i], 10);
        print(&b, " 00000 n \n");
    }
    print(&b, "trailer\n<< /Size 12 /Root 1 0 R /Prev ");
    if (prev.data) {
        print(&b, prev);
    } else {
        print(&b, parseint(p).val + skew);
    }
    print(&b, " >>\nstartxref\n");
    print(&b, x);
    print(&b, "\n%%EOF\n");
    assert(b.cap == 1<<12);
    return finish(&b);
}

static void test_pdfdocument(Test *t)
{
    Arena a = t->perm;
    i32   n = 0;
    Str  *o = pdfbdoc(&a, &n);
    Str   all = "ok 3: page one|page two|page three";

    // Each way of writing it, and each damage, reads the same
    static struct { Str name; i32 flags; Str want; } const cases[] = {
        {"pdf.doc",           0,                                "ok 3:"},
        {"pdf.doc lf19",      PDFB_LF19,                        "ok 3:"},
        {"pdf.doc junk",      PDFB_JUNK,                        "ok 3:"},
        {"pdf.doc prevloop",  PDFB_PREVLOOP,                    "ok 3:"},
        {"pdf.doc xrefstm",   PDFB_XREFSTM,                     "ok 3:"},
        {"pdf.doc objstm",    PDFB_OBJSTM,                      "ok 3:"},
        {"pdf.doc hybrid",    PDFB_HYBRID,                      "ok 3:"},
        {"pdf.doc stmloop",   PDFB_XREFSTM|PDFB_PREVLOOP,       "ok 3:"},
        {"pdf.doc shift",     PDFB_SHIFT,                       "ok 3 rec:"},
        {"pdf.doc none",      PDFB_XREFNONE,                    "ok 3 rec:"},
        {"pdf.doc sx0",       PDFB_SX0,                         "ok 3 rec:"},
        {"pdf.doc sxpast",    PDFB_SXPAST,                      "ok 3 rec:"},
        {"pdf.doc objstm shift", PDFB_OBJSTM|PDFB_SHIFT,        "ok 3 rec:"},
        {"pdf.doc objstm none",  PDFB_OBJSTM|PDFB_XREFNONE,     "ok 3 rec:"},
        {"pdf.doc hybrid shift", PDFB_HYBRID|PDFB_SHIFT,        "ok 3 rec:"},
        {"pdf.doc junk none",    PDFB_JUNK|PDFB_XREFNONE,       "ok 3 rec:"},
        {"pdf.doc junk sx0",     PDFB_JUNK|PDFB_SX0,            "ok 3 rec:"},
    };
    for (i32 i = 0; i < countof(cases); i++) {
        Str pdf  = pdfbuild(&a, o, n, cases[i].flags);
        Str want = concat(&a, cases[i].want, cuthead(all, 5));
        expect(t, cases[i].name, pdfsummary(&a, pdf), want);
    }

    // Which load only by repair (pdfdamaged), case by case: junk before
    // the header, a /Prev loop, and each recovered by the scan; of the
    // document with its /Count made true
    {
        Str *fix = alloc<Str>(&a, n);
        for (i32 i = 0; i < n; i++) fix[i] = o[i];
        iz at = find(fix[1], "/Count 99");
        assert(at >= 0);
        fix[1] = concat(&a, takehead(fix[1], at), concat(&a, "/Count 3",
                        cuthead(fix[1], at+9)));
        expect(t, "pdf.damaged doc", cut(pdfsummary(&a, pdfbuild(&a, fix, n, 0)), ':').head,
               "ok 3");

        Buf b(&a, 64);
        for (i32 i = 0; i < countof(cases); i++) {
            Str pdf = pdfbuild(&a, fix, n, cases[i].flags);
            putbyte(&b, (u8)('0' + pdfdamaged(a, pdf)));
        }
        expect(t, "pdf.damaged", finish(&b), "00110001111111111");

        // A page tree that counts pages it lacks, or no count at all
        Str good = pdfbuild(&a, fix, n, 0);
        iz  c    = find(good, "/Count 3");
        assert(c >= 0);
        Str lies = clone(&a, good);
        lies.data[c+7] = '9';
        Str none = clone(&a, good);
        none.data[c+1] = 'X';  // /Xount
        i32 r = pdfdamaged(a, lies)*100 + pdfdamaged(a, none)*10 + pdfdamaged(a, "nope");
        expect(t, "pdf.damaged count", istr(&a, r), "110");
    }

    // Each object resolves, from the table or the scan, the same
    {
        Str  pdfs[] = {pdfbuild(&a, o, n, 0), pdfbuild(&a, o, n, PDFB_SHIFT),
                       pdfbuild(&a, o, n, PDFB_OBJSTM), pdfbuild(&a, o, n, PDFB_HYBRID)};
        Str  prints[countof(pdfs)] = {};
        for (i32 k = 0; k < countof(pdfs); k++) {
            Buf   b(&a, 1<<12);
            Arena s = a;
            Pdf   p = pdfstart(s, pdfs[k], {});
            pdfload(&p);
            for (i32 i = 1; i <= n; i++) {
                pdfprintobj(&b, pdfobject(&p, i));
                putbyte(&b, '\n');
            }
            assert(b.cap == 1<<12);
            prints[k] = finish(&b);
        }
        expect(t, "pdf.resolve shift",  prints[1], prints[0]);
        expect(t, "pdf.resolve objstm", prints[2], prints[0]);
        expect(t, "pdf.resolve hybrid", prints[3], prints[0]);
        expect(t, "pdf.resolve 8", cut(cuthead(prints[0], find(prints[0],
               "<</Length 11")), '\n').head, "<</Length 11 0 R>> stream");
    }

    // What pages inherit: Resources and MediaBox from above, unless
    // their own; Rotate normalized
    {
        Str      pdf = pdfbuild(&a, o, n, 0);
        Buf      b(&a, 1024);
        Arena    s = a;
        Pdf      p = pdfstart(s, pdf, {});
        pdfload(&p);
        PdfPages g = pdfpages(&p);
        for (iz i = 0; i < g.pages.len; i++) {
            PdfPage pg = g.pages.data[i];
            print(&b, (i64)pg.num);
            putbyte(&b, ' ');
            pdfprintobj(&b, pg.mediabox);
            putbyte(&b, ' ');
            print(&b, (i64)pg.rotate);
            putbyte(&b, ' ');
            pdfprintobj(&b, pdfkey(pg.resources, "Font"));
            putbyte(&b, ';');
        }
        assert(b.cap == 1024);
        expect(t, "pdf.inherit", finish(&b),
               "3 [0 0 612 792] 90 <</F1 9 0 R>>;5 [0 0 100 100] 0 <</F1 9 0 R>>;"
               "7 [0 0 100 100] 90 <</F1 9 0 R>>;");
    }

    // Hand-made oddities
    Str hdr = "%PDF-1.4\n";
    expect(t, "pdf.notpdf", pdfsummary(&a, "<html>"), "notpdf 0:");
    expect(t, "pdf.empty", pdfsummary(&a, ""), "notpdf 0:");
    expect(t, "pdf.header only", pdfsummary(&a, hdr), "ok 0 rec fb:");

    // Pages found without a tree, in file order, inheriting by /Parent
    expect(t, "pdf.no tree", pdfsummary(&a, concat(&a, hdr,
           "2 0 obj << /Type /Pages /Resources << >> >> endobj\n"
           "4 0 obj << /Type /Page /Parent 2 0 R /Contents 5 0 R >> endobj\n"
           "3 0 obj << /Type /Page /Contents 6 0 R >> endobj\n"
           "5 0 obj << /Length 1 >> stream\nA\nendstream endobj\n"
           "6 0 obj << >> stream\nB\nendstream endobj\n")),
           "ok 2 rec fb: A|B");

    // A stream that says it is a page is not one (found by AFL)
    expect(t, "pdf.stream page", pdfsummary(&a, concat(&a, hdr,
           "3 0 obj << /Type /Page /Contents 6 0 R >> stream\nX\nendstream endobj\n"
           "4 0 obj << /Type /Page /Contents 6 0 R >> endobj\n"
           "6 0 obj << >> stream\nB\nendstream endobj\n")),
           "ok 1 rec fb: B");

    // A tree that lists itself, and one nested past the limit
    expect(t, "pdf.cycle", pdfsummary(&a, concat(&a, hdr,
           "1 0 obj << /Pages 2 0 R >> endobj\n"
           "2 0 obj << /Type /Pages /Kids [2 0 R 3 0 R 2 0 R 3 0 R] >> endobj\n"
           "3 0 obj << /Type /Page >> endobj\n"
           "trailer << /Root 1 0 R >>")), "ok 1 rec: ");
    {
        // Node i, from 2, holds node i+1 and page i+100, which reads i
        Buf b(&a, 1<<14);
        print(&b, hdr);
        print(&b, "1 0 obj << /Pages 2 0 R >> endobj\n");
        for (i32 i = 2; i < 80; i++) {
            print(&b, (i64)i);
            print(&b, " 0 obj << /Kids [");
            print(&b, (i64)i+1);
            print(&b, " 0 R ");
            print(&b, (i64)i+100);
            print(&b, " 0 R] >> endobj\n");
            print(&b, (i64)i+100);
            print(&b, " 0 obj << /Contents ");
            print(&b, (i64)i+200);
            print(&b, " 0 R >> endobj\n");
            print(&b, (i64)i+200);
            print(&b, " 0 obj << >> stream\n");
            print(&b, (i64)i);
            print(&b, "\nendstream endobj\n");
        }
        print(&b, "trailer << /Root 1 0 R >>");
        Str got = pdfsummary(&a, finish(&b));
        expect(t, "pdf.deep tree", takehead(got, 14), "ok 64 rec: 65|");
        expect(t, "pdf.deep tree last", taketail(got, 6), "|4|3|2");
    }

    // A stream whose /Length is itself, or whose object is its own
    // /Length's container: the cycle is broken, endstream found
    expect(t, "pdf.length self", pdfsummary(&a, concat(&a, hdr,
           "1 0 obj << /Pages 2 0 R >> endobj\n"
           "2 0 obj << /Kids [3 0 R] >> endobj\n"
           "3 0 obj << /Contents 4 0 R >> endobj\n"
           "4 0 obj << /Length 4 0 R >> stream\r\nself\r\nendstream endobj\n"
           "trailer << /Root 1 0 R >>")), "ok 1 rec: self");

    // Lengths that lie, long, short, huge or negative
    {
        Str lens[] = {"0", "3", "6", "1000", "999999999999", "-5", "(x)"};
        Buf b(&a, 256);
        for (i32 i = 0; i < countof(lens); i++) {
            Str d = concat(&a, hdr, "1 0 obj << /Pages 2 0 R >> endobj\n"
                    "2 0 obj << /Kids [3 0 R] >> endobj\n"
                    "3 0 obj << /Contents 4 0 R >> endobj\n4 0 obj << /Length ");
            d = concat(&a, d, lens[i]);
            d = concat(&a, d, " >> stream\nabcdef\nendstream endobj\ntrailer << /Root 1 0 R >>");
            print(&b, cuthead(pdfsummary(&a, d), 10));
            putbyte(&b, ';');
        }
        expect(t, "pdf.lengths", finish(&b), "abcdef;abcdef;abcdef;abcdef;abcdef;abcdef;abcdef;");
    }

    // An object stream holding itself, with /Extends itself: nothing
    // resolves through it, and nothing loops
    expect(t, "pdf.objstm self", pdfsummary(&a, concat(&a, hdr,
           "1 0 obj << /Pages 2 0 R >> endobj\n"
           "2 0 obj << /Kids [3 0 R] >> endobj\n"
           "3 0 obj << /Type /Page >> endobj\n"
           "5 0 obj << /Type /ObjStm /N 1 /First 4 /Extends 5 0 R >> stream\n"
           "5 0 << /Type /ObjStm >>\nendstream endobj\n"
           "trailer << /Root 1 0 R >>")), "ok 1 rec: ");

    // Encrypted, by a trailer or, found by the scan, by an encryption
    // dict; not by /Encrypt in a page's text
    {
        Str enc = concat(&a, hdr,
            "1 0 obj << /Pages 2 0 R >> endobj\n"
            "2 0 obj << /Kids [3 0 R] >> endobj\n"
            "3 0 obj << /Type /Page /Contents 4 0 R >> endobj\n"
            "4 0 obj << /Length 18 >> stream\n(/Encrypt 9 0 R) '\nendstream endobj\n");
        Str lock = "9 0 obj << /Filter /Standard /V 2 /R 3 /O <00> /U <01> /P -4 >> endobj\n";
        expect(t, "pdf.encrypt text", pdfsummary(&a, concat(&a, enc,
               "trailer << /Root 1 0 R >>")), "ok 1 rec: (/Encrypt 9 0 R) '");
        expect(t, "pdf.encrypt trailer", cut(pdfsummary(&a, concat(&a, enc,
               "trailer << /Root 1 0 R /Encrypt 9 0 R >>")), ' ').head, "encrypted");
        expect(t, "pdf.encrypt dict", cut(pdfsummary(&a, concat(&a, enc, lock)), ' ').head,
               "encrypted");
        Str good   = pdfbuild(&a, o, n, 0);
        Str locked = concat(&a, enc, "trailer << /Root 1 0 R /Encrypt 9 0 R >>");
        i32 r      = pdflocked(a, locked)*10 + pdflocked(a, good);
        expect(t, "pdf.locked", istr(&a, r), "10");
    }

    // An incremental update: the newer page wins, and the older entries
    // stand where the newer section says nothing
    {
        Str base = pdfbuild(&a, o, n, 0);
        iz  sx   = find(base, "startxref\n") + 10;
        Str prev = cut(cuthead(base, sx), '\n').head;
        Buf b(&a, 1024);
        print(&b, base);
        iz  at = b.len;
        print(&b, "6 0 obj\n");
        print(&b, pdfbstream(&a, "", "page one, revised"));
        print(&b, "\nendobj\n");
        iz  x = b.len;
        print(&b, "xref\n0 1\n0000000000 65535 f \n6 1\n");
        printpad(&b, at, 10);
        print(&b, " 00000 n \ntrailer\n<< /Size 12 /Root 1 0 R /Prev ");
        print(&b, prev);
        print(&b, " >>\nstartxref\n");
        print(&b, x);
        print(&b, "\n%%EOF\n");
        expect(t, "pdf.incremental", pdfsummary(&a, finish(&b)),
               "ok 3: page one, revised|page two|page three");
    }

    // A section with more rows than it holds, and an xref stream whose
    // /W is no good or /Index runs past its data
    expect(t, "pdf.xref short", pdfsummary(&a, concat(&a, hdr,
           "1 0 obj << /Pages 2 0 R >> endobj\n"
           "2 0 obj << /Kids [] >> endobj\n"
           "xref\n0 99999999\n0000000000 65535 f \n0000000009 00000 n \n0000000043 00000 n \n"
           "trailer << /Root 1 0 R >>\nstartxref\n73\n%%EOF")), "ok 0 fb:");

    // Found by review: each of these read wrongly, trapped, or stopped
    // short of what the file holds
    {
        Str page = "<< /Type /Page /Parent 2 0 R >>";
        Str tree[] = {"<< /Type /Catalog /Pages 2 0 R >>",
                      "<< /Type /Pages /Kids [3 0 R] /Count 1 >>", page};

        // A subsection numbered from the largest i64: no overflow
        {
            Str body = concat(&a, hdr, "1 0 obj << /Pages 2 0 R >> endobj\n"
                                       "2 0 obj << /Kids [] >> endobj\n");
            Str x    = "xref\n9223372036854775807 2\n0000000000 00000 n \n"
                       "0000000000 00000 n \ntrailer << /Root 1 0 R >>\nstartxref\n";
            Str pdf  = concat(&a, concat(&a, body, x), istr(&a, body.len));
            expect(t, "pdf.xref huge num", pdfsummary(&a, pdf), "ok 0 rec fb:");
        }

        // startxref naming a huge object: it is not parsed to tell
        {
            Buf b(&a, 1<<20);
            print(&b, hdr);
            for (i32 i = 0; i < 3; i++) {
                print(&b, (i64)i+1);
                print(&b, " 0 obj ");
                print(&b, tree[i]);
                print(&b, " endobj\n");
            }
            iz at = b.len;
            print(&b, "4 0 obj[");
            for (i32 i = 0; i < 200000; i++) print(&b, "1 ");
            print(&b, "]endobj\nstartxref\n");
            print(&b, at);
            print(&b, "\n%%EOF\n");
            assert(b.cap == 1<<20);
            expect(t, "pdf.startxref huge", pdfsummary(&a, finish(&b)), "ok 1 rec: ");
        }

        // A flood of "endstream" does not exhaust the scan's memory
        {
            Buf b(&a, 1<<22);
            print(&b, hdr);
            for (i32 i = 0; i < 3; i++) {
                print(&b, (i64)i+1);
                print(&b, " 0 obj ");
                print(&b, tree[i]);
                print(&b, " endobj\n");
            }
            print(&b, "%");
            for (i32 i = 0; i < 300000; i++) print(&b, "endstream");
            assert(b.cap == 1<<22);
            expect(t, "pdf.endstream flood", pdfsummary(&a, finish(&b)), "ok 1 rec: ");
        }

        // A /Prev that leads nowhere: older objects come from the scan
        {
            Str base = pdfbuild(&a, o, n, 0);
            expect(t, "pdf.prev good", pdfsummary(&a, pdfrevise(&a, base, 0)),
                   "ok 1: page one");
            expect(t, "pdf.prev skewed", pdfsummary(&a, pdfrevise(&a, base, 40)),
                   "ok 1: page one");
            expect(t, "pdf.prev past", pdfsummary(&a, pdfrevise(&a, base, 0, "99999999")),
                   "ok 1: page one");
        }

        // Blanks between "stream" and its end of line
        {
            Str z   = pdfbzlib(&a, "BT (hi) Tj ET");
            Buf b(&a, 1024);
            print(&b, hdr);
            print(&b, "1 0 obj << /Pages 2 0 R >> endobj\n"
                      "2 0 obj << /Kids [3 0 R] >> endobj\n"
                      "3 0 obj << /Contents 4 0 R >> endobj\n"
                      "4 0 obj << /Filter /FlateDecode /Length ");
            print(&b, z.len);
            print(&b, " >> stream \t\r\n");
            print(&b, z);
            print(&b, "\r\nendstream endobj\ntrailer << /Root 1 0 R >>");
            expect(t, "pdf.stream blanks", pdfsummary(&a, finish(&b)), "ok 1 rec: BT (hi) Tj ET");

            // The scan jumps over such data too: no page inside it
            Str in = "q 6 0 obj << /Type /Page >> endobj Q";
            Buf c(&a, 1024);
            print(&c, hdr);
            print(&c, "4 0 obj << /Type /Page /Contents 5 0 R >> endobj\n"
                      "5 0 obj << /Length ");
            print(&c, in.len);
            print(&c, " >> stream  \n");
            print(&c, in);
            print(&c, "\nendstream endobj\n");
            expect(t, "pdf.stream blanks scan", pdfsummary(&a, finish(&c)),
                   concat(&a, "ok 1 rec fb: ", in));
        }

        // Indirect numbers in decode parameters and an object stream
        {
            Str z = pdfbzlib(&a, "\x02hello");
            Buf b(&a, 1024);
            print(&b, hdr);
            print(&b, "1 0 obj << /Pages 2 0 R >> endobj\n"
                      "2 0 obj << /Kids [3 0 R] >> endobj\n"
                      "3 0 obj << /Contents 4 0 R >> endobj\n"
                      "5 0 obj 12 endobj\n6 0 obj 5 endobj\n"
                      "4 0 obj << /Filter /FlateDecode /DecodeParms "
                      "<< /Predictor 5 0 R /Columns 6 0 R >> /Length ");
            print(&b, z.len);
            print(&b, " >> stream\n");
            print(&b, z);
            print(&b, "\nendstream endobj\ntrailer << /Root 1 0 R >>");
            expect(t, "pdf.indirect parms", pdfsummary(&a, finish(&b)), "ok 1 rec: hello");

            Str head = "1 0 2 33 3 65 ";
            Str body = "<< /Type /Catalog /Pages 2 0 R >>"
                       "<< /Type /Pages /Kids [3 0 R] >>"
                       "<< /Type /Page >>";
            Buf c(&a, 1024);
            print(&c, hdr);
            print(&c, "8 0 obj 3 endobj\n9 0 obj ");
            print(&c, head.len);
            print(&c, " endobj\n7 0 obj << /Type /ObjStm /N 8 0 R /First 9 0 R /Length ");
            print(&c, head.len + body.len);
            print(&c, " >> stream\n");
            print(&c, head);
            print(&c, body);
            print(&c, "\nendstream endobj\n");
            expect(t, "pdf.indirect objstm", pdfsummary(&a, finish(&c)), "ok 1 rec: ");
        }

        // A PNG predictor whose pixel is more than a byte, not whole bytes
        {
            Str row = "\x01\x12\x34\x44";  // Sub, two bytes per pixel
            Str got = pdfdec(&a, "<</Filter/Fl/DecodeParms<</Predictor 11 /Colors 3 "
                                 "/BitsPerComponent 4 /Columns 2>>>>", pdfbzlib(&a, row)).data;
            expect(t, "pdf.predict bpp", got, "\x12\x34\x56");
        }

        // Junk before the header, offsets counted from the start of the
        // file, its xref in the last junk-length bytes: no recovery
        {
            Buf j(&a, 4096);
            for (i32 i = 0; i < 120; i++) print(&j, "Junk from a mail gateway.\r\n");
            Str pdf = pdfhand(&a, finish(&j), tree, 3, "");
            expect(t, "pdf.junk absolute", pdfsummary(&a, pdf), "ok 1: ");
        }

        // Encrypted by a loaded trailer or xref stream, and by an xref
        // stream found only by recovery
        {
            Str got = pdfsummary(&a, pdfbuild(&a, o, n, PDFB_ENCRYPT));
            expect(t, "pdf.encrypt xref", cut(got, ' ').head, "encrypted");
            got = pdfsummary(&a, pdfbuild(&a, o, n, PDFB_ENCRYPT|PDFB_XREFSTM));
            expect(t, "pdf.encrypt xrefstm", cut(got, ' ').head, "encrypted");
            got = pdfsummary(&a, pdfbuild(&a, o, n, PDFB_ENCRYPT|PDFB_XREFSTM|PDFB_SHIFT));
            expect(t, "pdf.encrypt xrefstm rec", got, "encrypted 3 rec:"
                   " page one|page two|page three");
        }

        // A catalog without /Type, found by the trailer that a scanned
        // xref stream holds
        {
            Str *p2 = alloc<Str>(&a, n);
            for (i32 i = 0; i < n; i++) p2[i] = o[i];
            p2[0] = "<< /Pages 2 0 R >>";
            Str got = pdfsummary(&a, pdfbuild(&a, p2, n, PDFB_XREFSTM|PDFB_SHIFT));
            expect(t, "pdf.rec xrefstm", got, "ok 3 rec: page one|page two|page three");
        }

        // An object the xref puts at an offset where it is not, but
        // which an object stream holds
        {
            Str base = pdfbuild(&a, o, n, PDFB_OBJSTM);
            iz  sx   = find(base, "startxref\n") + 10;
            Str prev = cut(cuthead(base, sx), '\n').head;
            Buf b(&a, 1<<12);
            print(&b, base);
            iz  x = b.len;
            print(&b, "xref\n0 1\n0000000000 65535 f \n3 1\n0000000000 00000 n \n"
                      "trailer\n<< /Size 12 /Root 1 0 R /Prev ");
            print(&b, prev);
            print(&b, " >>\nstartxref\n");
            print(&b, x);
            print(&b, "\n%%EOF\n");
            assert(b.cap == 1<<12);
            expect(t, "pdf.misplaced in objstm", pdfsummary(&a, finish(&b)), all);
        }
    }

    // The sweeps, over the plain file, an xref stream with an object
    // stream, and a hybrid
    {
        i32 fl[] = {0, PDFB_OBJSTM, PDFB_HYBRID};
        Str nm[] = {"plain", "objstm", "hybrid"};
        for (i32 i = 0; i < countof(fl); i++) {
            Str pdf   = pdfbuild(&a, o, n, fl[i]);
            iz  whole = fl[i] ? find(pdf, " 0 obj\n<< /Type /XRef") - 2 : find(pdf, "\nxref\n") + 1;
            Str name  = concat(&a, "pdf.sweep cut ", nm[i]);
            expect(t, name, pdfcutsweep(&a, pdf, whole, all), "");
            name = concat(&a, "pdf.sweep memory ", nm[i]);
            expect(t, name, istr(&a, pdfmemsweep(&a, pdf, all)), "1");
            name = concat(&a, "pdf.sweep work ", nm[i]);
            expect(t, name, istr(&a, pdfworksweep(&a, pdf, all)), "1");
        }
        Str pdf = pdfbuild(&a, o, n, 0);
        expect(t, "pdf.sweep corrupt", istr(&a, pdfcorruptsweep(&a, pdf, all)), "317");
        pdf = pdfbuild(&a, o, n, PDFB_OBJSTM);
        expect(t, "pdf.sweep corrupt objstm", istr(&a, pdfcorruptsweep(&a, pdf, all)), "482");
    }
}
