// PDF tests: telling a PDF by its first bytes, and counting its pages
// by the root's /Count, by its page objects when it has none, and in
// object streams (deflated by png.cpp's encoder, as Wuffs inflates
// them), and spotting encryption; damaged files count none.

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
