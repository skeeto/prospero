// PDF: only as much as sending one to a model needs (read, engine.cpp):
// whether a file is one, how many pages it has, and whether it is
// encrypted, from its bytes, without parsing it whole. Pages kept in
// object streams (PDF 1.5) are found by inflating those streams. The
// file need not be trusted: every scan stays inside the bytes.

enum : i32 {
    PDF_HEAD   = 1024,       // "%PDF-" may come this far in
    PDF_OBJSTM = 16 << 20,   // largest object stream inflated

    // Sent to a model (Anthropic's document blocks): a request takes at
    // most 100 pages and 32 MB, which base64 leaves room for 16 MiB of
    // PDFs in, the largest a file may be too. A page is estimated at its
    // image (measured near 1600 tokens) and its text together.
    PDF_PAGES  = 100,
    PDF_BYTES  = 16 << 20,
    PDF_TOKENS = 2000,       // per page
};

struct PdfInfo {
    i32 pages;      // the page count, 0 if none was found
    b32 encrypted;
};

// Whether a file beginning with head is a PDF.
static b32 pdfsniff(Str head)
{
    return find(takehead(head, head.len<PDF_HEAD ? head.len : PDF_HEAD), "%PDF-") >= 0;
}

static b32 pdfspace(u8 c)
{
    return c==0 || c=='\t' || c=='\n' || c=='\f' || c=='\r' || c==' ';
}

static b32 pdfdelim(u8 c)
{
    return pdfspace(c) || c=='(' || c==')' || c=='<' || c=='>' || c=='[' ||
           c==']' || c=='{' || c=='}' || c=='/' || c=='%';
}

// The index just past name (as "/Type") at i in d, if it is there and
// ends there, else -1.
static iz pdfname(Str d, iz i, Str name)
{
    if (i<0 || d.len-i<name.len || !(Str(d.data+i, name.len)==name)) {
        return -1;
    }
    i += name.len;
    return i==d.len || pdfdelim(d[i]) ? i : -1;
}

static iz pdfskip(Str d, iz i)
{
    for (; i<d.len && pdfspace(d[i]); i++) {}
    return i;
}

// The extent [beg, end) of the dictionary around i, bounded to a window
// around it, or beg<0 if none is found.
struct PdfDict {
    iz beg;
    iz end;
};

static PdfDict pdfdict(Str d, iz i)
{
    iz      window = 1 << 16;
    PdfDict r      = {-1, -1};
    i32     depth  = 0;
    for (iz j = i-1; j>=1 && i-j<window; j--) {
        if (d[j-1]=='>' && d[j]=='>') {
            depth++;
            j--;
        } else if (d[j-1]=='<' && d[j]=='<') {
            if (!depth--) {
                r.beg = j - 1;
                break;
            }
            j--;
        }
    }
    if (r.beg < 0) {
        return r;
    }
    depth = 0;
    for (iz j = i; j+1<d.len && j-i<window; j++) {
        if (d[j]=='<' && d[j+1]=='<') {
            depth++;
            j++;
        } else if (d[j]=='>' && d[j+1]=='>') {
            if (!depth--) {
                r.end = j + 2;
                return r;
            }
            j++;
        }
    }
    r.beg = -1;
    return r;
}

// The /Count of the dictionary [beg, end), its own and not a nested
// one's, or 0 if none.
static i64 pdfcount(Str d, PdfDict k)
{
    i32 depth = 0;
    for (iz j = k.beg+2; j < k.end-2; j++) {
        if (d[j]=='<' && d[j+1]=='<') {
            depth++;
            j++;
        } else if (d[j]=='>' && d[j+1]=='>') {
            depth--;
            j++;
        } else if (!depth && d[j]=='/') {
            iz at = pdfname(d, j, "/Count");
            if (at < 0) {
                continue;
            }
            i64 n = 0;
            at = pdfskip(d, at);
            for (; at<k.end && d[at]>='0' && d[at]<='9' && n<1000000; at++) {
                n = n*10 + d[at] - '0';
            }
            return n;
        }
    }
    return 0;
}

// What a scan of some bytes found: the largest /Count of a /Pages
// dictionary, which the root's is, and the /Page objects.
struct PdfScan {
    i64 count;
    i64 objects;
    b32 encrypted;
};

static void pdfscan(Str d, PdfScan *r)
{
    for (iz i = 0; i < d.len; i++) {
        if (d[i] != '/') {
            continue;
        } else if (pdfname(d, i, "/Encrypt") >= 0) {
            r->encrypted = 1;
            continue;
        }
        iz at = pdfname(d, i, "/Type");
        if (at < 0) {
            continue;
        }
        at = pdfskip(d, at);
        if (pdfname(d, at, "/Page") >= 0) {
            r->objects++;
        } else if (pdfname(d, at, "/Pages") >= 0) {
            PdfDict k = pdfdict(d, i);
            i64     n = k.beg>=0 ? pdfcount(d, k) : 0;
            r->count = n>r->count ? n : r->count;
        }
    }
}

// Inflate each Flate-encoded object stream in d and scan it. Its data
// runs from "stream" to "endstream", each line end trimmed, and is zlib:
// a two-byte header, then DEFLATE, then a trailer left unread.
static void pdfobjstms(Arena scratch, Str d, PdfScan *r)
{
    Str key = "/ObjStm";
    for (iz i = 0; i+key.len <= d.len; i++) {
        if (d[i]!='/' || pdfname(d, i, key)<0) {
            continue;
        }
        PdfDict k = pdfdict(d, i);
        if (k.beg < 0) {
            continue;
        }
        Str dict = Str(d.data+k.beg, k.end-k.beg);
        if (find(dict, "/FlateDecode")<0 || find(dict, "/DecodeParms")>=0) {
            continue;  // another filter, or predictors
        }
        iz at = pdfskip(d, k.end);
        if (pdfname(d, at, "stream") < 0) {
            continue;
        }
        at += 6;
        at += at<d.len && d[at]=='\r';
        at += at<d.len && d[at]=='\n';
        Str rest = cuthead(d, at);
        iz  end  = find(rest, "endstream");
        if (end < 3) {
            continue;
        }
        Str data = takehead(rest, end);
        iz  cap  = data.len*16<PDF_OBJSTM ? data.len*16 : PDF_OBJSTM;
        for (;;) {
            // Fresh, zeroed state each time, and an output that fills
            // to cap only if the stream wants more than that
            Arena t     = scratch;
            iz    n     = wuffsinflatestate();
            u8   *mem   = alloc<u8>(&t, n+15);
            void *state = (void *)(((uz)mem + 15) & ~(uz)15);
            u8   *out   = alloc<u8>(&t, cap);
            iz    len   = 0;
            char const *err = wuffsinflateto(state, out, cap, &len,
                                             data.data+2, data.len-2);
            if (err && len==cap && cap<PDF_OBJSTM) {
                cap = cap*4<PDF_OBJSTM ? cap*4 : PDF_OBJSTM;
                continue;
            } else if (!err) {
                pdfscan(Str(out, len), r);
            }
            break;
        }
        i = at + data.len;
    }
}

static PdfInfo pdfinfo(Arena scratch, Str d)
{
    PdfScan s = {};
    pdfscan(d, &s);
    if (!s.encrypted) {
        pdfobjstms(scratch, d, &s);
    }
    PdfInfo r = {};
    i64     n = s.count ? s.count : s.objects;
    r.pages     = n>0x7fffffff ? 0x7fffffff : (i32)n;
    r.encrypted = s.encrypted;
    return r;
}
