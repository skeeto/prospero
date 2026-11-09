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
    return find(takehead(head, head.len<PDF_HEAD ? head.len : (iz)PDF_HEAD), "%PDF-") >= 0;
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
        iz  cap  = data.len*16<PDF_OBJSTM ? data.len*16 : (iz)PDF_OBJSTM;
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
                cap = cap*4<PDF_OBJSTM ? cap*4 : (iz)PDF_OBJSTM;
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


// Reader: limits
//
// The reader below extracts a document's text and finds its images. It
// takes no Os, so tests and the fuzzer run it as the engine does, and it
// trusts nothing in the file: every size taken from it is checked before
// it is allocated, and failing a check stops the read with a reason,
// never os_oom. Every loop over file data consumes a byte or charges a
// unit of a budget, and budgets are counts, not clocks, so one input
// always gives the same output.

enum : i64 {
    PDF_DEPTH        = 64,             // nested arrays and dicts, as JSON_MAXDEPTH
    PDF_ITEMS        = 1 << 18,        // items in one array or dict
    PDF_OBJNUM       = (1 << 23) - 1,  // largest object number
    PDF_XREFS        = 1 << 20,        // index entries kept
    PDF_SECTIONS     = 256,            // xref sections, by /Prev and /XRefStm
    PDF_HOPS         = 8,              // nested resolutions
    PDF_FILTERS      = 8,              // filters in one chain
    PDF_STREAMMAX    = 64 << 20,       // bytes one stream decodes to
    PDF_PAGEMAX      = 1 << 16,        // pages walked
    PDF_TREEDEPTH    = 64,             // page tree depth
    PDF_FORMDEPTH    = 16,             // nested forms
    PDF_QDEPTH       = 32,             // nested q
    PDF_OPERANDS     = 64,             // operands kept for one operator
    PDF_GLYPHS       = 1 << 18,        // per page
    PDF_PLACES       = 4096,           // image placements per page...
    PDF_PLACESALL    = 1 << 16,        // ...and in all
    PDF_FONTS        = 1024,
    PDF_CMAPRANGES   = 1 << 16,        // kept as ranges, never expanded
    PDF_OUTLINES     = 1000,
    PDF_TEXTMAX      = sizeof(iz)==4 ? 4<<20 : 16<<20,
    PDF_IMGEDGE      = 1 << 15,
    PDF_DECODEPIXELS = sizeof(iz)==4 ? 1<<22 : 1<<25,
    PDF_READMAX      = sizeof(iz)==4 ? 16<<20 : 64<<20,
    PDF_REGION       = sizeof(iz)==4 ? 160<<20 : 1<<30,

    PDF_RUN          = 64,             // regular characters lexed as one BAD token
    PDF_HEADSCAN     = 1 << 20,        // the reader looks this far for %PDF-...
    PDF_TAILSCAN     = 1 << 20,        // ...and this far back for startxref

    // Budgets, scaled by the input's length and clamped
    PDF_WORK         = 64,             // per byte: tokens, operators, glyphs, rows
    PDF_WORKMIN      = 1 << 22,
    PDF_WORKMAX      = 1 << 26,
    PDF_DECODED      = 256,            // per byte: all filter output
    PDF_DECODEDMIN   = 16 << 20,
    PDF_DECODEDMAX   = 512 << 20,
    PDF_SCANNED      = 4,              // per byte, plus PDF_SCANNEDMIN: recovery
    PDF_SCANNEDMIN   = 16 << 20,

    // Arenas, carved from the reader's scratch: keep and op are scaled
    // like the budgets, and page takes the rest
    PDF_KEEPMIN      = 8 << 20,        // plus 8 per byte
    PDF_KEEPMAX      = sizeof(iz)==4 ? 32<<20 : 128<<20,
    PDF_OPMIN        = 4 << 20,        // plus 1 per byte
    PDF_OPMAX        = 64 << 20,
};


// Reader: object values

enum : i32 {
    PDFO_NULL,
    PDFO_BOOL,
    PDFO_INT,
    PDFO_REAL,
    PDFO_STR,
    PDFO_NAME,
    PDFO_ARRAY,   // len items
    PDFO_DICT,    // len pairs of items, a name then its value
    PDFO_REF,
    PDFO_STREAM,
};

struct PdfRef {
    i32 num;
    i32 gen;
};

struct PdfStream;

struct PdfObj {
    i32 kind;
    i32 len;
    union {
        i64        i = 0;
        b32        b;
        f64        r;
        Str        s;
        PdfRef     ref;
        PdfObj    *items;
        PdfStream *st;
    };
};
static_assert(sizeof(PdfObj) <= 24);

// A stream: its dict, and its data as the file holds it, still encoded.
struct PdfStream {
    PdfObj dict;
    Str    raw;
};


// Reader: context, budgets and charged allocation

// Budgets tighter than the defaults (tests, the fuzzer); zero is default.
struct PdfLimits {
    i64 work    = 0;
    i64 decoded = 0;
    i64 scanned = 0;
};

// One read of one document. Its three arenas differ by lifetime: keep
// lasts the document (index, memoized objects, fonts, CMaps), page a page
// (glyphs, page text, placements, decoded contents), and op an operator.
// Only op is ever rewound (pdfmark), so nothing kept can be freed under
// what keeps it.
struct PdfSlot;
struct PdfIndex;

struct Pdf {
    Str   data;
    Arena keep;
    Arena page;
    Arena op;
    i64   work;     // units left
    i64   decoded;  // bytes left
    i64   scanned;  // bytes left
    Str   why;      // set, a literal, once the read must stop
    i32   damage;   // local repairs made

    // The document, once loaded (pdfload)
    PdfSlot  *xref;       // the xref sections' entries
    PdfIndex *index;      // the recovery scan's, made at most once
    PdfObj    trailer;    // the newest trailer
    PdfObj    root;       // the catalog, as the trailer names it
    PdfObj    info;
    iz        head;       // where %PDF- is
    iz        base;       // what xref offsets count from: 0 or head
    i32       entries;    // xref entries kept, at most PDF_XREFS
    i32       hops;       // resolutions in progress
    u32       epoch;      // marks objects visited by a walk
    b32       scanned1;   // the scan was tried
    b32       encrypted;
    b32       recovered;  // the structure came from the scan
    b32       cut;        // an older xref section could not be read
};

static i64 pdfclamp(i64 v, i64 lo, i64 hi)
{
    return v<lo ? lo : v>hi ? hi : v;
}

// Bytes that can still be allocated from a, counting uncommitted space.
static iz pdfroom(Arena *a)
{
    byte *limit = a->region ? a->region->base+a->region->cap : a->end;
    return limit - a->beg;
}

// A reader over data, its arenas carved from scratch: keep and op are
// limited to a share of it, and page takes the rest.
static Pdf pdfstart(Arena scratch, Str data, PdfLimits lim)
{
    Pdf r  = {};
    i64 n  = data.len;
    r.data = data;

    iz room = pdfroom(&scratch);
    iz keep = (iz)pdfclamp(PDF_KEEPMIN + 8*n, PDF_KEEPMIN, PDF_KEEPMAX);
    keep    = keep<room/2 ? keep : room/2;
    r.keep  = carve(&scratch, keep);
    iz op   = (iz)pdfclamp(PDF_OPMIN + n, PDF_OPMIN, PDF_OPMAX);
    op      = op<room/4 ? op : room/4;
    r.op    = carve(&scratch, op);
    r.page  = scratch;

    r.work    = lim.work    ? lim.work    : pdfclamp(n*PDF_WORK, PDF_WORKMIN, PDF_WORKMAX);
    r.decoded = lim.decoded ? lim.decoded : pdfclamp(n*PDF_DECODED, PDF_DECODEDMIN, PDF_DECODEDMAX);
    r.scanned = lim.scanned ? lim.scanned : n*PDF_SCANNED + PDF_SCANNEDMIN;
    return r;
}

// Stop the read, keeping the first reason. Always returns 0.
static b32 pdfstop(Pdf *p, Str why)
{
    if (!p->why.data) {
        p->why = why;
    }
    return 0;
}

// Charge work units, or stop the read when they run out.
static b32 pdfcharge(Pdf *p, i64 units)
{
    if (p->why.data) {
        return 0;
    } else if (units > p->work) {
        p->work = 0;
        return pdfstop(p, "it needs more work than is allowed");
    }
    p->work -= units;
    return 1;
}

// Whether count objects of size bytes after pad fit in a, as alloc
// requires them to: it needs strictly more room than it takes.
static b32 pdffits(Arena *a, iz pad, iz count, iz size)
{
    return count>=0 && count<(pdfroom(a) - pad)/size;
}

// Allocate n zeroed objects, or stop the read and return null.
template<typename T>
static T *pdfnew(Pdf *p, Arena *a, iz n)
{
    iz pad = -(uz)a->beg & (alignof(T) - 1);
    if (p->why.data) {
        return 0;
    } else if (!pdffits(a, pad, n, sizeof(T))) {
        pdfstop(p, "out of memory");
        return 0;
    }
    return alloc<T>(a, n);
}

// Make room for need more bytes in b, as reserve would, or stop the read.
// Growing costs the larger of need and its capacity, or that plus a copy
// of it when b is not at its arena's front.
static b32 pdfreserve(Pdf *p, Buf *b, iz need)
{
    if (p->why.data) {
        return 0;
    } else if (need<0) {
        return pdfstop(p, "out of memory");
    } else if (b->cap-b->len >= need) {
        return 1;
    }
    Arena *a      = b->a;
    iz     extend = b->cap>need ? b->cap : need;
    b32    front  = (byte *)(b->data+b->cap) == a->beg;
    iz     room   = pdfroom(a);
    if (extend>=room || (!front && b->cap>=room-extend)) {
        return pdfstop(p, "out of memory");
    }
    reserve(b, need);
    return 1;
}

// Append v to s, as push would, or stop the read and return s unchanged.
// A full slice not at its arena's front is copied before it grows.
template<typename T>
static Slice<T> pdfpush(Pdf *p, Arena *a, Slice<T> s, T v)
{
    if (p->why.data) {
        return s;
    } else if (s.len == s.cap) {
        iz  pad    = -(uz)a->beg & (alignof(T) - 1);
        iz  extend = s.cap ? s.cap : 8;
        b32 front  = s.data && (byte *)(s.data+s.cap)==a->beg;
        iz  count  = front ? extend : s.cap+extend+1;
        if (!pdffits(a, pad, count, sizeof(T))) {
            pdfstop(p, "out of memory");
            return s;
        }
    }
    return push(a, s, v);
}

// Sort, as sort does, its scratch taken from a: or stop the read, the
// items left as they were. Each pass costs a unit per item.
template<typename T>
static b32 pdfsort(Pdf *p, Arena *a, T *v, iz n, b32 (*less)(T *, T *))
{
    iz  pad    = -(uz)a->beg & (alignof(T) - 1);
    i64 passes = 1;
    for (iz w = 1; w < n; w *= 2) {
        passes++;
    }
    if (p->why.data) {
        return 0;
    } else if (n>1 && !pdffits(a, pad, n, sizeof(T))) {
        return pdfstop(p, "out of memory");
    } else if (!pdfcharge(p, passes*n)) {
        return 0;
    }
    sort(v, n, less, *a);
    return 1;
}

// A point in op to rewind to, after an operator's temporaries.
static Arena pdfmark(Pdf *p)
{
    return p->op;
}

// By pointer, so that the arena checker sees the mark given back as a
// copy of op (a restore), not a stranger, which would drop what op
// held before the mark too.
static void pdfrewind(Pdf *p, Arena *mark)
{
    p->op = *mark;
}


// Reader: lexer

enum : i32 {
    PDFT_EOF,
    PDFT_INT,
    PDFT_REAL,
    PDFT_STR,     // s: its bytes, escapes decoded
    PDFT_NAME,    // s: its bytes, without the slash, #xx decoded
    PDFT_KEY,     // s: a keyword or operator, in the input even if unstable
    PDFT_AOPEN,   // [
    PDFT_ACLOSE,  // ]
    PDFT_DOPEN,   // <<
    PDFT_DCLOSE,  // >>
    PDFT_BOPEN,   // {
    PDFT_BCLOSE,  // }
    PDFT_BAD,     // a stray ) or >, or a run of PDF_RUN regular characters
};

struct PdfTok {
    i32 kind;
    iz  beg;  // [beg, end) in the lexer's input, never empty but at EOF
    iz  end;
    i64 i;
    f64 r;
    Str s;
};

// A lexer over d, from i. Decoded strings and names go in a. Only when
// d outlives what a holds (stable) may tokens point into d: the file
// does, but a decoded stream may not. Skipping decodes nothing.
struct PdfLex {
    Pdf   *p;
    Arena *a;
    Str    d;
    iz     i;
    b32    stable;
    b32    skip;
};

static i32 pdfhex(u8 c)  // -1 if not a hex digit
{
    if (c>='0' && c<='9') return c - '0';
    if (c>='a' && c<='f') return c - 'a' + 10;
    if (c>='A' && c<='F') return c - 'A' + 10;
    return -1;
}

// A real as an integer, clamped, with NaN as zero.
static i64 pdftoint(f64 f)
{
    return f!=f ? 0 : f>=9e18 ? (i64)9e18 : f<=-9e18 ? -(i64)9e18 : (i64)f;
}

struct PdfNum {
    i64 i;
    f64 r;
    b32 real;
};

// A number, leniently: any signs ("--5" is -5), digits with an optional
// point on either side (".5", "4."), and an optional exponent. Trailing
// junk is ignored, no digits is 0, and an integer too large for i64 is a
// real. Reals stay finite, at most 1e300 in magnitude.
static PdfNum pdfnum(Str s)
{
    static f64 const pow10[] = {
        1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
        1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22,
    };

    PdfNum r   = {};
    iz     i   = 0;
    b32    neg = 0;
    for (; i<s.len && (s[i]=='-' || s[i]=='+'); i++) {
        neg |= s[i] == '-';
    }

    u64 m    = 0;  // significant digits
    i32 n    = 0;  // number of them kept, at most 19
    i32 e    = 0;  // decimal exponent of m
    i64 v    = 0;  // the integer, accumulated negatively
    b32 over = 0;
    for (; i<s.len && digit(s[i]); i++) {
        i32 d = s[i] - '0';
        over |= v < (I64MIN + d)/10;
        v = over ? v : v*10 - d;
        if (n < 19) {
            m = m*10 + (u64)d;
            n += m != 0;
        } else {
            e++;
        }
    }
    over |= !neg && v==I64MIN;
    r.real = over;

    if (i<s.len && s[i]=='.') {
        r.real = 1;
        for (i++; i<s.len && digit(s[i]); i++) {
            if (n < 19) {
                m = m*10 + (u64)(s[i] - '0');
                n += m != 0;
                e--;
            }
        }
    }

    if (i+1<s.len && (s[i]=='e' || s[i]=='E')) {
        iz  j    = i + 1;
        b32 eneg = s[j] == '-';
        j += s[j]=='-' || s[j]=='+';
        if (j<s.len && digit(s[j])) {
            r.real = 1;
            i32 x = 0;
            for (; j<s.len && digit(s[j]); j++) {
                x = x<100000 ? x*10 + (s[j] - '0') : x;  // saturate
            }
            e += eneg ? -x : x;
        }
    }

    if (!r.real) {
        r.i = neg ? v : -v;
        r.r = (f64)r.i;
        return r;
    }

    f64 f = 0;
    if (m && e+n-1>300) {
        f = 1e300;
    } else if (m && e+n-1>=-300) {
        f = (f64)m;
        for (; e >  22; e -= 22) f *= 1e22;
        for (; e < -22; e += 22) f /= 1e22;
        f = e<0 ? f/pow10[-e] : f*pow10[e];
    }
    r.r = neg ? -f : f;
    r.i = pdftoint(r.r);
    return r;
}

// Skip whitespace and comments. Returns the next byte, or -1 at the end.
static i32 pdfpeek(PdfLex *x)
{
    Str d = x->d;
    while (x->i < d.len) {
        u8 c = d[x->i];
        if (pdfspace(c)) {
            x->i++;
        } else if (c == '%') {
            for (; x->i<d.len && d[x->i]!='\n' && d[x->i]!='\r'; x->i++) {}
        } else {
            return c;
        }
    }
    return -1;
}

// A buffer of up to n bytes for a decoded token, or null (stopped).
static u8 *pdftokbuf(PdfLex *x, iz n)
{
    return x->skip ? 0 : pdfnew<u8>(x->p, x->a, n);
}

// The body of a literal string, (beg, end) excluding its parentheses,
// decoded: escapes, line continuations, and raw CR or CRLF as LF.
static Str pdflitstr(PdfLex *x, iz beg, iz end, b32 plain)
{
    Str d   = x->d;
    Str raw = Str(d.data+beg, end-beg);
    if (plain && x->stable) {
        return raw;
    }
    u8 *out = pdftokbuf(x, raw.len);
    if (!out) {
        return {};
    }
    iz n = 0;
    for (iz i = 0; i < raw.len;) {
        u8 c = raw[i++];
        if (c == '\r') {
            i += i<raw.len && raw[i]=='\n';
            out[n++] = '\n';
            continue;
        } else if (c!='\\') {
            out[n++] = c;
            continue;
        } else if (i == raw.len) {
            break;
        }
        c = raw[i++];
        switch (c) {
        case 'n': out[n++] = '\n'; break;
        case 'r': out[n++] = '\r'; break;
        case 't': out[n++] = '\t'; break;
        case 'b': out[n++] = '\b'; break;
        case 'f': out[n++] = '\f'; break;
        case '\r': i += i<raw.len && raw[i]=='\n'; break;
        case '\n': break;
        default:
            if (c>='0' && c<='7') {
                i32 v = c - '0';
                for (i32 k = 0; k<2 && i<raw.len && raw[i]>='0' && raw[i]<='7'; k++) {
                    v = v*8 + raw[i++] - '0';
                }
                out[n++] = (u8)v;  // "\777" wraps, as readers do
            } else {
                out[n++] = c;      // unknown escapes drop the backslash
            }
        }
    }
    return {out, n};
}

static PdfTok pdflex(PdfLex *x)
{
    PdfTok r = {};
    Str    d = x->d;
    i32    c = pdfpeek(x);
    r.beg = r.end = x->i;
    if (c<0 || !pdfcharge(x->p, 1)) {
        return r;  // EOF
    }

    iz i = x->i;
    switch (c) {
    case '[': r.kind = PDFT_AOPEN;  i++; break;
    case ']': r.kind = PDFT_ACLOSE; i++; break;
    case '{': r.kind = PDFT_BOPEN;  i++; break;
    case '}': r.kind = PDFT_BCLOSE; i++; break;
    case ')': r.kind = PDFT_BAD;    i++; break;

    case '>':
        i++;
        r.kind = PDFT_BAD;
        if (i<d.len && d[i]=='>') {
            r.kind = PDFT_DCLOSE;
            i++;
        }
        break;

    case '<':
        i++;
        if (i<d.len && d[i]=='<') {
            r.kind = PDFT_DOPEN;
            i++;
            break;
        }
        // A hex string: junk skipped, an odd digit padded, open at EOF
        r.kind = PDFT_STR;
        {
            iz beg = i;
            for (; i<d.len && d[i]!='>'; i++) {}
            iz  len = i - beg;
            i += i < d.len;
            u8 *out = pdftokbuf(x, (len+1)/2);
            if (out) {
                iz n = 0;
                for (iz j = 0; j < len; j++) {
                    i32 v = pdfhex(d[beg+j]);
                    if (v >= 0) {
                        out[n/2] = (u8)(out[n/2] | v<<(n&1 ? 0 : 4));
                        n++;
                    }
                }
                r.s = {out, (n+1)/2};
            }
        }
        break;

    case '(':
        // Balanced parentheses, escapes skipped over, open at EOF
        r.kind = PDFT_STR;
        {
            i32 depth = 1;
            b32 plain = 1;
            iz  beg   = ++i;
            iz  end   = d.len;
            for (; i < d.len; i++) {
                u8 b = d[i];
                if (b == '\\') {
                    plain = 0;
                    i += i+1 < d.len;
                } else if (b == '\r') {
                    plain = 0;
                } else if (b == '(') {
                    depth++;
                } else if (b==')' && !--depth) {
                    end = i++;
                    break;
                }
            }
            r.s = pdflitstr(x, beg, end, plain);
        }
        break;

    case '/':
        r.kind = PDFT_NAME;
        {
            iz  beg   = ++i;
            b32 plain = 1;
            for (; i<d.len && !pdfdelim(d[i]); i++) {
                plain &= d[i] != '#';
            }
            Str raw = Str(d.data+beg, i-beg);
            if (plain && x->stable) {
                r.s = raw;
            } else if (u8 *out = pdftokbuf(x, raw.len)) {
                iz n = 0;
                for (iz j = 0; j < raw.len; j++) {
                    i32 hi = raw[j]=='#' && j+2<raw.len ? pdfhex(raw[j+1]) : -1;
                    i32 lo = hi>=0 ? pdfhex(raw[j+2]) : -1;
                    if (lo >= 0) {
                        out[n++] = (u8)(hi<<4 | lo);
                        j += 2;
                    } else {
                        out[n++] = raw[j];  // a lone '#' is kept
                    }
                }
                r.s = {out, n};
            }
        }
        break;

    default:
        // Regular characters: a number, a keyword, or a run too long
        // for either
        {
            iz beg = i;
            for (; i<d.len && !pdfdelim(d[i]); i++) {}
            Str s = Str(d.data+beg, i-beg);
            if (s.len >= PDF_RUN) {
                r.kind = PDFT_BAD;
            } else if (digit(s[0]) || s[0]=='+' || s[0]=='-' || s[0]=='.') {
                PdfNum v = pdfnum(s);
                r.kind = v.real ? PDFT_REAL : PDFT_INT;
                r.i    = v.i;
                r.r    = v.r;
            } else {
                r.kind = PDFT_KEY;
                r.s    = s;
            }
        }
    }

    assert(i > x->i);
    r.end = x->i = i;
    return r;
}


// Reader: objects

static PdfObj pdfparsetok(PdfLex *, PdfTok, i32 depth);

// Skip what is left of depth nested arrays and dicts, counting brackets
// without recursing or decoding, so that nesting of any depth costs
// time linear in its length, and constant space.
static void pdfskipnest(PdfLex *x, i32 depth)
{
    b32 skip = x->skip;
    x->skip = 1;
    while (depth > 0) {
        PdfTok t = pdflex(x);
        switch (t.kind) {
        case PDFT_EOF:    depth = 0; break;
        case PDFT_AOPEN:
        case PDFT_DOPEN:  depth++;   break;
        case PDFT_ACLOSE:
        case PDFT_DCLOSE: depth--;   break;
        }
    }
    x->skip = skip;
}

// Whether a reference's "G R" follows, from x's position: consumed if so.
// Only an integer and a keyword are lexed ahead, so nothing is allocated.
static PdfObj pdfrefafter(PdfLex *x, i64 num)
{
    PdfObj r    = {};
    iz     save = x->i;
    if (num>0 && num<=PDF_OBJNUM && digit((u8)pdfpeek(x))) {
        PdfTok g = pdflex(x);
        if (g.kind==PDFT_INT && g.i>=0 && g.i<=0xffff && pdfpeek(x)=='R') {
            PdfTok k = pdflex(x);
            if (k.kind==PDFT_KEY && k.s=="R") {
                r.kind    = PDFO_REF;
                r.ref.num = (i32)num;
                r.ref.gen = (i32)g.i;
                return r;
            }
        }
    }
    x->i = save;
    return r;
}

// The items of an array or dict, up to its closing token, into x->a.
// Items past PDF_ITEMS are parsed but dropped. In a dict, a key that is
// not a name is dropped, and a missing value is null.
static PdfObj pdfparseitems(PdfLex *x, i32 kind, i32 depth)
{
    Pdf          *p     = x->p;
    i32           close = kind==PDFO_ARRAY ? PDFT_ACLOSE : PDFT_DCLOSE;
    Slice<PdfObj> items = {};
    for (;;) {
        PdfTok t = pdflex(x);
        if (t.kind==close || t.kind==PDFT_EOF) {
            break;
        }
        PdfObj key = {};
        if (kind == PDFO_DICT) {
            if (t.kind != PDFT_NAME) {
                p->damage++;
                pdfparsetok(x, t, depth+1);
                continue;
            }
            key.kind = PDFO_NAME;
            key.s    = t.s;
            t = pdflex(x);
        }
        b32    end = t.kind==close || t.kind==PDFT_EOF;
        PdfObj v   = end ? PdfObj{} : pdfparsetok(x, t, depth+1);
        if (items.len >= (kind==PDFO_DICT ? 2*PDF_ITEMS : PDF_ITEMS)) {
            p->damage++;
        } else if (kind == PDFO_DICT) {
            items = pdfpush(p, x->a, items, key);
            items = pdfpush(p, x->a, items, v);
        } else {
            items = pdfpush(p, x->a, items, v);
        }
        if (end) {
            break;
        }
    }
    PdfObj r = {};
    r.kind  = kind;
    r.len   = (i32)(kind==PDFO_DICT ? items.len/2 : items.len);
    r.items = items.data;
    return r;
}

// The object that begins with token t. Containers nested deeper than
// PDF_DEPTH are skipped as null, and so is any token that begins no
// object: a keyword but true, false and null, or a stray closer.
static PdfObj pdfparsetok(PdfLex *x, PdfTok t, i32 depth)
{
    PdfObj r = {};
    switch (t.kind) {
    case PDFT_INT:
        r = pdfrefafter(x, t.i);
        if (!r.kind) {
            r.kind = PDFO_INT;
            r.i    = t.i;
        }
        break;
    case PDFT_REAL:
        r.kind = PDFO_REAL;
        r.r    = t.r;
        break;
    case PDFT_STR:
        r.kind = PDFO_STR;
        r.s    = t.s;
        break;
    case PDFT_NAME:
        r.kind = PDFO_NAME;
        r.s    = t.s;
        break;
    case PDFT_KEY:
        if (t.s=="true" || t.s=="false") {
            r.kind = PDFO_BOOL;
            r.b    = t.s == "true";
        }
        break;
    case PDFT_AOPEN:
    case PDFT_DOPEN:
        if (depth >= PDF_DEPTH) {
            x->p->damage++;
            pdfskipnest(x, 1);
            break;
        }
        r = pdfparseitems(x, t.kind==PDFT_AOPEN ? PDFO_ARRAY : PDFO_DICT, depth);
        break;
    }
    return r;
}

// The next object from x, null at its end.
static PdfObj pdfparse(PdfLex *x)
{
    return pdfparsetok(x, pdflex(x), 0);
}

// The value of a dict's key, the first if repeated, or null. A stream
// stands for its dict.
static PdfObj pdfkey(PdfObj dict, Str name)
{
    dict = dict.kind==PDFO_STREAM ? dict.st->dict : dict;
    if (dict.kind == PDFO_DICT) {
        for (i32 i = 0; i < dict.len; i++) {
            if (dict.items[2*i].s == name) {
                return dict.items[2*i+1];
            }
        }
    }
    return {};
}

// A real in canonical form, zero unsigned: at most six decimals, and
// at most 14 digits in all, so that it parses back to a real that
// prints the same. Larger magnitudes take an exponent and 7 digits.
static void pdfprintreal(Buf *b, f64 v)
{
    f64 mag      = __builtin_fabs(v);
    i32 decimals = 6;
    f64 scale    = 1e6;
    if (mag >= 1e14) {
        printfloat(b, v, 6);
        return;
    }
    for (; decimals && mag*scale>=1e14; decimals--) {
        scale /= 10;
    }
    if (mag*1e6 < 0.5) {
        putbyte(b, '0');
    } else {
        printfloat(b, v, decimals);
    }
}

static void pdfprintstr(Buf *b, Str s)
{
    putbyte(b, '(');
    for (iz i = 0; i < s.len; i++) {
        u8 c = s[i];
        switch (c) {
        case '\n': print(b, "\\n"); break;
        case '\r': print(b, "\\r"); break;
        case '\t': print(b, "\\t"); break;
        case '\b': print(b, "\\b"); break;
        case '\f': print(b, "\\f"); break;
        case '(': case ')': case '\\':
            putbyte(b, '\\');
            putbyte(b, c);
            break;
        default:
            if (c<0x20 || c>0x7e) {
                putbyte(b, '\\');
                putbyte(b, (u8)('0' + (c>>6)));
                putbyte(b, (u8)('0' + (c>>3 & 7)));
                putbyte(b, (u8)('0' + (c    & 7)));
            } else {
                putbyte(b, c);
            }
        }
    }
    putbyte(b, ')');
}

static void pdfprintname(Buf *b, Str s)
{
    putbyte(b, '/');
    for (iz i = 0; i < s.len; i++) {
        u8 c = s[i];
        if (c<=0x20 || c>0x7e || c=='#' || pdfdelim(c)) {
            putbyte(b, '#');
            printhex(b, c, 2);
        } else {
            putbyte(b, c);
        }
    }
}

// An object as canonical text, which parses back to the same object:
// for tests, the fuzzer, and notes.
static void pdfprintobj(Buf *b, PdfObj o)
{
    switch (o.kind) {
    case PDFO_NULL: print(b, "null"); break;
    case PDFO_BOOL: print(b, o.b ? Str("true") : "false"); break;
    case PDFO_INT:  print(b, o.i); break;
    case PDFO_REAL: pdfprintreal(b, o.r); break;
    case PDFO_STR:  pdfprintstr(b, o.s); break;
    case PDFO_NAME: pdfprintname(b, o.s); break;
    case PDFO_REF:
        print(b, (i64)o.ref.num);
        putbyte(b, ' ');
        print(b, (i64)o.ref.gen);
        print(b, " R");
        break;
    case PDFO_ARRAY:
        putbyte(b, '[');
        for (i32 i = 0; i < o.len; i++) {
            if (i) putbyte(b, ' ');
            pdfprintobj(b, o.items[i]);
        }
        putbyte(b, ']');
        break;
    case PDFO_DICT:
        print(b, "<<");
        for (i32 i = 0; i < o.len; i++) {
            if (i) putbyte(b, ' ');
            pdfprintobj(b, o.items[2*i]);
            putbyte(b, ' ');
            pdfprintobj(b, o.items[2*i+1]);
        }
        print(b, ">>");
        break;
    case PDFO_STREAM:
        pdfprintobj(b, o.st->dict);  // the document's, never parsed here
        print(b, " stream");
        break;
    }
}


// Reader: filters
//
// A stream's filters run in order, each stage's output capped and
// charged to the decoded budget. The in-house decoders (LZW, ASCIIHex,
// ASCII85, RunLength, the predictors) decode a prefix of their input to
// a prefix of their whole output, so a stream cut short still gives what
// it holds: a partial final group or code is used only at its EOD. The
// image filters end a chain, left to the image decoders with their
// parameters.

enum : i32 {
    PDFF_UNKNOWN,
    PDFF_FLATE,
    PDFF_LZW,
    PDFF_AHX,
    PDFF_A85,
    PDFF_RL,
    PDFF_CRYPT,
    PDFF_DCT,     // the image filters, from here on
    PDFF_CCITT,
    PDFF_JPX,
    PDFF_JBIG2,
};

// Image filters left to apply, as PdfData reports them.
enum : i32 {
    PDFI_NONE,
    PDFI_DCT,
    PDFI_CCITT,
    PDFI_JPX,
    PDFI_JBIG2,
};

struct PdfData {
    Str    data;   // null when a filter could not be applied
    i32    image;  // PDFI_*: an image filter still to apply, with parms
    PdfObj parms;  // that filter's /DecodeParms
    b32    cut;    // stopped at a cap, the output so far kept
};

// A filter's name, or an inline image's abbreviation of it.
static i32 pdffilterid(Str name)
{
    static struct { Str name, abbr; i32 id; } const filters[] = {
        {"FlateDecode",     "Fl",  PDFF_FLATE},
        {"LZWDecode",       "LZW", PDFF_LZW},
        {"ASCIIHexDecode",  "AHx", PDFF_AHX},
        {"ASCII85Decode",   "A85", PDFF_A85},
        {"RunLengthDecode", "RL",  PDFF_RL},
        {"Crypt",           {},    PDFF_CRYPT},
        {"DCTDecode",       "DCT", PDFF_DCT},
        {"CCITTFaxDecode",  "CCF", PDFF_CCITT},
        {"JPXDecode",       {},    PDFF_JPX},
        {"JBIG2Decode",     {},    PDFF_JBIG2},
    };
    for (iz i = 0; i < countof(filters); i++) {
        if (name==filters[i].name || (filters[i].abbr.len && name==filters[i].abbr)) {
            return filters[i].id;
        }
    }
    return PDFF_UNKNOWN;
}

static PdfObj pdfget(Pdf *, PdfObj);
static b32    pdfcharge(Pdf *, i64);

// pdfkey, its scan charged: a file may repeat a lookup without bound.
static PdfObj pdfkeyc(Pdf *p, PdfObj dict, Str name)
{
    PdfObj d = dict.kind==PDFO_STREAM ? dict.st->dict : dict;
    if (d.kind==PDFO_DICT && d.len>16 && !pdfcharge(p, d.len/16)) {
        return {};
    }
    return pdfkey(dict, name);
}

// A number from a dict, resolved, as an integer, or def if absent or not
// a number.
static i64 pdfint(Pdf *p, PdfObj dict, Str key, i64 def)
{
    PdfObj v = pdfget(p, pdfkey(dict, key));
    switch (v.kind) {
    case PDFO_INT:  return v.i;
    case PDFO_REAL: return pdftoint(v.r);
    }
    return def;
}

// One stage's output, growing at the front of its arena up to cap bytes.
// Nothing else may allocate from that arena while it grows.
struct PdfSink {
    Pdf *p;
    Buf  b;
    iz   cap;
    b32  full;  // more was wanted than cap
};

static PdfSink pdfsink(Pdf *p, Arena *a, iz cap)
{
    PdfSink r = {};
    r.p   = p;
    r.b.a = a;
    r.cap = cap;
    return r;
}

// Make room for up to n more bytes, returning how many may be written:
// fewer once the cap is reached, none if the read stopped. Growth
// doubles, never past the cap.
static iz pdfgrow(PdfSink *o, iz n)
{
    Pdf *p    = o->p;
    Buf *b    = &o->b;
    iz   left = o->cap - b->len;
    if (n > left) {
        o->full = 1;
        n = left;
    }
    if (p->why.data) {
        return 0;
    } else if (b->cap-b->len >= n) {
        return n;
    }

    Arena *a    = b->a;
    iz     lack = n - (b->cap - b->len);
    iz     grow = b->cap ? b->cap : 4096;
    grow = grow>lack ? grow : lack;
    grow = grow<o->cap-b->cap ? grow : o->cap-b->cap;
    b32 front = b->data && (byte *)(b->data+b->cap)==a->beg;
    iz  cost  = front ? grow : b->cap+grow;
    if (cost >= pdfroom(a)) {
        pdfstop(p, "out of memory");
        return 0;
    }
    if (front) {
        allocbytes(a, grow);
    } else {
        u8 *data = allocbytes(a, b->cap+grow);
        copybytes(data, b->data, b->len);
        b->data = data;
    }
    b->cap += grow;
    return n;
}

// Append c: or return 0, o full or the read stopped.
static b32 pdfput(PdfSink *o, u8 c)
{
    if (o->b.len==o->b.cap && pdfgrow(o, 1)<1) {
        return 0;
    }
    o->b.data[o->b.len++] = c;
    return 1;
}

// The output, its unused tail given back when it ends at the front.
static Str pdfsinkdone(PdfSink *o)
{
    Buf   *b = &o->b;
    Arena *a = b->a;
    if (b->data && (byte *)(b->data+b->cap)==a->beg) {
        a->beg  = (byte *)(b->data + b->len);  // trim
        b->cap  = b->len;
    }
    return {b->len ? b->data : 0, b->len};
}

// Inflate zlib, or raw DEFLATE when its header is not zlib's. A guess
// at the size grows fourfold up to the cap, each attempt charged, so
// retries cost at most a third more than the last. A stream damaged or
// cut short keeps what inflated before the fault.
static Str pdfinflate(PdfSink *o, Str in)
{
    Pdf   *p = o->p;
    Arena *a = o->b.a;
    if (in.len>=2 && (in[0]&15)==8 && in[0]>>4<=7 &&
            (in[0]*256 + in[1])%31==0 && !(in[1]&0x20)) {
        in = cuthead(in, 2);
    }

    iz cap   = o->cap;
    iz guess = in.len<cap/8 ? in.len*8 : cap;
    guess = guess>4096 ? guess : 4096;
    guess = guess<cap ? guess : cap;
    for (;;) {
        if (p->why.data || guess>=pdfroom(a)) {
            pdfstop(p, "out of memory");
            return {};
        }
        Arena mark  = pdfmark(p);
        iz    n     = wuffsinflatestate();
        u8   *mem   = pdfnew<u8>(p, &p->op, n+15);
        if (!mem) {
            return {};
        }
        void *state = (void *)(((uz)mem + 15) & ~(uz)15);
        byte *beg   = a->beg;
        u8   *out   = allocbytes(a, guess);
        iz    len   = 0;
        char const *err = wuffsinflateto(state, out, guess, &len, in.data, in.len);
        pdfrewind(p, &mark);
        p->decoded -= len;

        b32 more = err && len==guess;
        if (more && guess<cap) {
            a->beg = beg;  // trim, for a larger attempt
            cap    = cap<p->decoded ? cap : (iz)p->decoded;
            guess  = guess<cap/4 ? guess*4 : cap;
            if (guess > len) {
                continue;
            }
            // The budget left is no more than this attempt gave: keep it
            out = allocbytes(a, len);
        }
        a->beg = (byte *)(out + len);  // trim
        o->full = more;
        p->damage += err && !more;
        return {len ? out : 0, len};
    }
}

// LZW as PDF has it: MSB-first codes of 9 to 12 bits, 256 clears the
// table and 257 ends the data. The width grows one code early unless
// early is 0. A full table stays at 12 bits and takes no more entries.
// The table lives in op, rewound after.
static Str pdflzw(PdfSink *o, Str in, b32 early)
{
    struct Entry {
        i32 prefix;
        i32 len;
        u8  last;
        u8  first;
    };
    Pdf   *p     = o->p;
    Arena  mark  = pdfmark(p);
    Entry *table = pdfnew<Entry>(p, &p->op, 4096);
    if (!table) {
        return {};
    }
    for (i32 i = 0; i < 256; i++) {
        table[i].len   = 1;
        table[i].last  = (u8)i;
        table[i].first = (u8)i;
    }

    i32 next  = 258;
    i32 width = 9;
    i32 prev  = -1;
    u32 acc   = 0;
    i32 nbits = 0;
    iz  i     = 0;
    for (;;) {
        for (; nbits<width && i<in.len; nbits += 8) {
            acc = acc<<8 | in[i++];
        }
        if (nbits < width) {
            break;  // a partial code at the end
        }
        nbits -= width;
        i32 code = (i32)(acc>>nbits & ((1u<<width) - 1));

        if (code == 256) {
            next  = 258;
            width = 9;
            prev  = -1;
            continue;
        } else if (code == 257) {
            break;
        } else if (code>next || (code==next && (prev<0 || next==4096))) {
            p->damage++;
            break;
        }

        // KwKwK: the code being defined is the previous string and its
        // own first byte, so add it before writing it
        b32 kwk = code == next;
        if (prev>=0 && next<4096) {
            Entry *e = table + next;
            e->prefix = prev;
            e->len    = table[prev].len + 1;
            e->first  = table[prev].first;
            e->last   = kwk ? table[prev].first : table[code].first;
            next++;
            width += next+early>=(1<<width) && width<12;
        }
        prev = code;

        // Written backwards along the prefix chain, the head only if
        // that is all that fits
        i32 len   = table[code].len;
        iz  avail = pdfgrow(o, len);
        u8 *dst   = o->b.data + o->b.len;
        for (i32 c = code, k = len-1; k >= 0; c = table[c].prefix, k--) {
            if (k < avail) {
                dst[k] = table[c].last;
            }
        }
        o->b.len += avail;
        if (avail < len) {
            break;
        }
    }
    pdfrewind(p, &mark);
    return pdfsinkdone(o);
}

// ASCIIHex: whitespace skipped, '>' ends it, an odd final digit there
// padded with 0. Any other byte stops it.
static Str pdfahx(PdfSink *o, Str in)
{
    i32 hi = -1;
    for (iz i = 0; i < in.len; i++) {
        u8  c = in[i];
        i32 v = pdfhex(c);
        if (c == '>') {
            if (hi >= 0) {
                pdfput(o, (u8)(hi<<4));
            }
            break;
        } else if (pdfspace(c)) {
            continue;
        } else if (v < 0) {
            o->p->damage++;
            break;
        } else if (hi < 0) {
            hi = v;
        } else if (!pdfput(o, (u8)(hi<<4 | v))) {
            break;
        } else {
            hi = -1;
        }
    }
    return pdfsinkdone(o);
}

// ASCII85: an optional "<~", whitespace skipped, 'z' for four zeros at
// a group's start, and '~' ends it, where a final group of 2-4 digits is
// padded with 'u'. A group past 2^32-1, or any other byte, stops it.
static Str pdfa85(PdfSink *o, Str in)
{
    Pdf *p = o->p;
    iz   i = 0;
    for (; i<in.len && pdfspace(in[i]); i++) {}
    if (i+1<in.len && in[i]=='<' && in[i+1]=='~') {
        i += 2;
    }

    u64 v = 0;
    i32 n = 0;
    for (; i < in.len; i++) {
        u8 c = in[i];
        if (pdfspace(c)) {
            continue;
        } else if (c == '~') {
            if (n == 1) {
                p->damage++;
            } else if (n > 1) {
                for (i32 k = n; k < 5; k++) {
                    v = v*85 + 84;
                }
                if (v > 0xffffffff) {
                    p->damage++;
                } else {
                    for (i32 k = 0; k<n-1 && pdfput(o, (u8)(v>>(24 - 8*k))); k++) {}
                }
            }
            break;
        } else if (c=='z' && !n) {
            b32 ok = 1;
            for (i32 k = 0; k<4 && (ok = pdfput(o, 0)); k++) {}
            if (!ok) {
                break;
            }
            continue;
        } else if (c<'!' || c>'u') {
            p->damage++;
            break;
        }
        v = v*85 + (u64)(c - '!');
        if (++n == 5) {
            if (v > 0xffffffff) {
                p->damage++;
                break;
            }
            b32 ok = 1;
            for (i32 k = 0; k<4 && (ok = pdfput(o, (u8)(v>>(24 - 8*k)))); k++) {}
            if (!ok) {
                break;
            }
            v = 0;
            n = 0;
        }
    }
    return pdfsinkdone(o);
}

// RunLength: 0-127 copies the next n+1 bytes, 129-255 repeats the next
// byte 257-n times, and 128 ends it.
static Str pdfrl(PdfSink *o, Str in)
{
    for (iz i = 0; i < in.len;) {
        i32 n = in[i++];
        if (n == 128) {
            break;
        }
        iz len  = n<128 ? n+1 : 257-n;
        iz have = n<128 ? in.len-i : i<in.len;
        if (!have) {
            break;
        }
        len = n<128 && have<len ? have : len;
        iz avail = pdfgrow(o, len);
        u8 *dst  = o->b.data + o->b.len;
        if (n < 128) {
            copybytes(dst, in.data+i, avail);
            i += len;
        } else {
            __builtin_memset(dst, in[i++], (uz)avail);
        }
        o->b.len += avail;
        if (avail < len) {
            break;
        }
    }
    return pdfsinkdone(o);
}

static i32 pdfpaeth(i32 a, i32 b, i32 c)
{
    i32 p  = a + b - c;
    i32 pa = p>a ? p-a : a-p;
    i32 pb = p>b ? p-b : b-p;
    i32 pc = p>c ? p-c : c-p;
    return pa<=pb && pa<=pc ? a : pb<=pc ? b : c;
}

// Undo a predictor in place, per /DecodeParms: TIFF's (2) or PNG's
// (10-15, each row naming its own, an unknown one taken as None). A
// partial last row is undone as far as it goes, in whole samples. Bad
// parameters give null, damaged.
static Str pdfpredict(Pdf *p, Str d, PdfObj parms)
{
    i64 pred   = pdfint(p, parms, "Predictor", 1);
    i64 colors = pdfint(p, parms, "Colors", 1);
    i64 bpc    = pdfint(p, parms, "BitsPerComponent", 8);
    i64 cols   = pdfint(p, parms, "Columns", 1);
    if (pred == 1) {
        return d;
    } else if ((pred!=2 && (pred<10 || pred>15)) ||
               colors<1 || colors>32 ||
               (bpc!=1 && bpc!=2 && bpc!=4 && bpc!=8 && bpc!=16) ||
               cols<1 || cols>(1<<24)) {
        p->damage++;
        return {};
    }
    i64 bits = colors * bpc;
    iz  rb   = (iz)((bits*cols + 7) / 8);  // at most 2^30
    if (rb > PDF_STREAMMAX) {
        p->damage++;
        return {};
    }
    u8 *s = d.data;

    if (pred >= 10) {
        iz bpp = (iz)((bits + 7) / 8);
        iz out = 0;
        for (iz in = 0; in < d.len; in += rb+1) {
            i32 type = s[in];
            iz  n    = d.len-in-1<rb ? d.len-in-1 : rb;
            u8 *row  = s + out;
            u8 *up   = out ? row-rb : 0;
            u8 *src  = s + in + 1;
            for (iz j = 0; j < n; j++) {
                i32 a = j>=bpp ? row[j-bpp] : 0;
                i32 b = up ? up[j] : 0;
                i32 c = up && j>=bpp ? up[j-bpp] : 0;
                i32 x = src[j];
                switch (type) {
                case 1: x += a;                    break;
                case 2: x += b;                    break;
                case 3: x += (a + b) / 2;          break;
                case 4: x += pdfpaeth(a, b, c);    break;
                }
                row[j] = (u8)x;
            }
            out += n;
        }
        return {out ? s : 0, out};
    }

    // TIFF: each sample adds the one colors samples before it in its row
    iz nc  = (iz)colors;  // at most 32, and cols at most 2^24, as checked
    iz len = d.len;
    if (bpc == 16) {
        len &= ~(iz)1;  // a lone half sample would change with the next byte
    }
    for (iz r = 0; r < len; r += rb) {
        iz  n   = len-r<rb ? len-r : rb;
        u8 *row = s + r;
        if (bpc == 8) {
            for (iz j = nc; j < n; j++) {
                row[j] = (u8)(row[j] + row[j-colors]);
            }
        } else if (bpc == 16) {
            for (iz j = 2*nc; j+1 < n; j += 2) {
                i32 v = (row[j]<<8 | row[j+1]) + (row[j-2*colors]<<8 | row[j-2*colors+1]);
                row[j]   = (u8)(v >> 8);
                row[j+1] = (u8)v;
            }
        } else {
            i32 b       = (i32)bpc;
            i32 mask    = (1<<b) - 1;
            iz  samples = nc * (iz)cols;
            samples = samples<n*8/b ? samples : n*8/b;
            for (iz j = nc; j < samples; j++) {
                iz  at   = j * b;
                iz  from = (j - nc) * b;
                i32 sh   = 8 - b - (i32)(at & 7);
                i32 fs   = 8 - b - (i32)(from & 7);
                i32 v    = (row[at>>3]>>sh) + (row[from>>3]>>fs);
                row[at>>3] = (u8)((row[at>>3] & ~(mask<<sh)) | (v&mask)<<sh);
            }
        }
    }
    return {len ? s : 0, len};
}

// One filter over in, its output in a and at most cap bytes, further
// limited by the decoded budget, by PDF_STREAMMAX and by half a's room,
// and charged to the budget. Flate and LZW take a predictor. The image
// filters and an unknown one give null, as does Crypt but Identity.
// The cut flag says the output stopped at the cap.
static PdfData pdffilter(Pdf *p, Arena *a, i32 id, PdfObj parms, Str in, iz cap)
{
    PdfData r = {};
    assert(a != &p->op);  // the decoders' temporaries go there
    iz room = pdfroom(a) / 2;
    cap = cap<PDF_STREAMMAX ? cap : (iz)PDF_STREAMMAX;
    cap = cap<p->decoded    ? cap : (iz)p->decoded;
    cap = cap<room          ? cap : room;
    cap = cap>0             ? cap : 0;
    if (p->why.data) {
        return r;
    }

    PdfSink o       = pdfsink(p, a, cap);
    b32    predict = 0;
    switch (id) {
    case PDFF_FLATE:
        r.data  = pdfinflate(&o, in);  // charges its own attempts
        predict = 1;
        break;
    case PDFF_LZW:
        r.data  = pdflzw(&o, in, pdfint(p, parms, "EarlyChange", 1) != 0);
        p->decoded -= r.data.len;
        predict = 1;
        break;
    case PDFF_AHX:
        r.data = pdfahx(&o, in);
        p->decoded -= r.data.len;
        break;
    case PDFF_A85:
        r.data = pdfa85(&o, in);
        p->decoded -= r.data.len;
        break;
    case PDFF_RL:
        r.data = pdfrl(&o, in);
        p->decoded -= r.data.len;
        break;
    case PDFF_CRYPT: {
        PdfObj name = pdfkey(parms, "Name");
        if (name.kind==PDFO_NULL || (name.kind==PDFO_NAME && name.s=="Identity")) {
            r.data = takehead(in, in.len<cap ? in.len : cap);
            r.cut  = in.len > cap;
            return r;
        }
    } [[fallthrough]];
    default:
        p->damage++;
        return r;
    }
    r.cut = o.full;
    if (predict) {
        r.data = pdfpredict(p, r.data, parms);
    }
    return r;
}

// A stream's data, from raw through the filters its dict names (/Filter,
// or an inline image's /F, with /DecodeParms or /DP aligned, a lone dict
// for every filter), in a and at most cap bytes: PDF_STREAMMAX if zero.
// Unfiltered data is raw itself. An image filter ends the chain, left to
// the image decoders with its parameters. A stage stopped at the stream
// limit is damage, one stopped by the decoded budget or memory stops the
// read; either way cut is set and its output so far is kept, unless the
// read stopped before the last stage, when there is none. Each stage
// moves down over the one before, so a chain holds at most two at once.
static PdfData pdfdecode(Pdf *p, Arena *a, PdfObj dict, Str raw, iz cap)
{
    PdfData r      = {};
    PdfObj  filter = pdfget(p, pdfkey(dict, "Filter"));
    PdfObj  parms  = pdfget(p, pdfkey(dict, "DecodeParms"));
    filter = filter.kind ? filter : pdfget(p, pdfkey(dict, "F"));
    parms  = parms.kind  ? parms  : pdfget(p, pdfkey(dict, "DP"));
    cap    = cap>0 && cap<PDF_STREAMMAX ? cap : (iz)PDF_STREAMMAX;

    i32     n     = 0;
    PdfObj *names = 0;
    switch (filter.kind) {
    case PDFO_NULL:  break;
    case PDFO_NAME:  n = 1; names = &filter; break;
    case PDFO_ARRAY: n = filter.len; names = filter.items; break;
    default:
        p->damage++;
        return r;
    }
    if (n > PDF_FILTERS) {
        p->damage++;
        return r;
    }

    // Resolved before any output: resolving parses into keep, which may
    // be where the output grows
    i32    ids[PDF_FILTERS] = {};
    PdfObj fps[PDF_FILTERS] = {};
    for (i32 i = 0; i < n; i++) {
        PdfObj name = pdfget(p, names[i]);
        PdfObj fp   = parms.kind==PDFO_ARRAY ? (i<parms.len ? parms.items[i] : PdfObj{})
                    : parms.kind==PDFO_DICT  ? parms : PdfObj{};
        ids[i] = name.kind==PDFO_NAME ? pdffilterid(name.s) : PDFF_UNKNOWN;
        fps[i] = pdfget(p, fp);
    }

    // The last stage decoded here takes the caller's cap, the rest the
    // stream limit, as an earlier stage's output is not the stream's
    i32 last = -1;
    for (i32 i = 0; i<n && ids[i]!=PDFF_UNKNOWN && ids[i]<PDFF_DCT; i++) {
        last = i;
    }

    r.data = takehead(raw, raw.len<cap ? raw.len : cap);
    r.cut  = raw.len>cap && last<0;  // nothing decoded here
    byte *base = a->beg;
    b32   ours = 0;  // r.data is in a, at base
    for (i32 i = 0; i<n && !p->why.data; i++) {
        i32    id = ids[i];
        PdfObj fp = fps[i];
        if (id == PDFF_UNKNOWN) {
            p->damage++;
            r.data = {};
            return r;
        } else if (id >= PDFF_DCT) {
            static i32 const kinds[] = {PDFI_DCT, PDFI_CCITT, PDFI_JPX, PDFI_JBIG2};
            r.image  = kinds[id-PDFF_DCT];
            r.parms  = fp;
            p->damage += i+1 < n;  // nothing decodes an image's output
            return r;
        }

        iz  stage = i==last ? cap : (iz)PDF_STREAMMAX;
        iz  room  = pdfroom(a) / 2;
        i64 left  = p->decoded;
        iz  c     = stage<left ? stage : (iz)left;
        c = c<room ? c : room;
        PdfData s = pdffilter(p, a, id, fp, i ? r.data : raw, c);
        if (id != PDFF_CRYPT) {
            if (ours && s.data.len) {
                __builtin_memmove(base, s.data.data, (uz)s.data.len);
                s.data.data = (u8 *)base;
            }
            if (ours) {
                a->beg = base + s.data.len;  // trim
            }
            ours = 1;
        }
        r.data = s.data;
        if (s.cut) {
            r.cut = 1;
            if (c == stage) {
                p->damage += stage == PDF_STREAMMAX;
            } else if (c==left || !p->decoded) {
                pdfstop(p, "it decodes to more than is allowed");
            } else {
                pdfstop(p, "out of memory");
            }
        }
        if (p->why.data && i<last) {
            // Stopped before the last stage: what an earlier stage gave
            // is not the stream's data, so none is kept
            a->beg = ours ? base : a->beg;
            r.data = {};
            return r;
        }
    }
    return r;
}


// Reader: document
//
// Loading finds the header, then the xref sections from startxref back,
// newest first, into an index by object number. When they give no
// catalog to use, one scan of the whole file finds the objects by their
// headers instead (recovery), and the trailer by what it finds. Objects
// are parsed when first resolved, and kept: from where the xref says,
// or from where the scan found them when the xref is wrong or silent.
// Streams are found in the file as they are, their filters left to
// pdfstream. The page tree gives every page, with what it inherits.
//
//   Pdf      p = pdfstart(scratch, data, limits);
//   i32      r = pdfload(&p);              // PDFR_OK, _NOTPDF, _ENCRYPTED
//   PdfPages g = pdfpages(&p);             // in order, perhaps none
//   PdfObj   v = pdfget(&p, obj);          // a reference resolved
//   PdfObj   w = pdfget(&p, dict, "Key");  // a key's value, resolved
//   PdfData  d = pdfstream(&p, &p.page, obj, cap);
//
// Everything kept goes in keep. A read that has to stop (budget, memory)
// says why in p.why, and what it has so far stays usable. pdflocked is
// the load alone, for the verdict on encryption.

enum : i32 {
    PDFR_OK,
    PDFR_PARTIAL,
    PDFR_NOTPDF,
    PDFR_ENCRYPTED,
    PDFR_NOPAGES,
};

enum : i32 {
    PDFS_FREE,
    PDFS_AT,  // at an offset in the file
    PDFS_IN,  // in an object stream
};

// An object stream's contents, decoded once, and where each object is.
struct PdfObjStm {
    Str  data;
    i32  n;
    i32 *nums;
    iz  *offs;  // into data
};

// Where an object is, and its value once resolved. Kept in a hash trie
// by number, sized by the entries it holds and never by /Size.
struct PdfSlot {
    PdfSlot   *child[4];
    i32        num;
    i32        gen;
    i32        kind;   // PDFS_*
    i32        rev;    // the xref section that made it, 1 the newest
    iz         off;    // AT: in data, -1 if outside it; IN: its index
    i32        in;     // IN: its container's number
    i32        state;  // 0 unresolved, 1 resolving, 2 resolved
    u32        epoch;  // a walk visited it
    PdfSlot   *box;    // IN, from the scan: its container
    PdfObjStm *stm;    // AT: its contents, if an object stream, once tried
    PdfObj     val;
};

static PdfSlot *pdfslot(Pdf *p, PdfSlot **m, i32 num, b32 create)
{
    for (u64 h = (u64)num * 0x9e3779b97f4a7c15u; *m; h <<= 2) {
        if ((*m)->num == num) {
            return *m;
        }
        m = &(*m)->child[h>>62];
    }
    if (!create) {
        return 0;
    }
    *m = pdfnew<PdfSlot>(p, &p->keep, 1);
    if (*m) {
        (*m)->num = num;
        (*m)->off = -1;
    }
    return *m;
}

// What the scan found, in file order: object headers (num, gen, flags),
// "endstream" and "trailer".
enum : i32 {
    PDFX_HEAD,
    PDFX_END,
    PDFX_TRAILER,
};

// What a scanned object's dict seems to hold, from its names alone.
enum : i32 {
    PDFH_OBJSTM  = 1 << 0,  // /Type /ObjStm
    PDFH_CATALOG = 1 << 1,
    PDFH_PAGE    = 1 << 2,
    PDFH_XREF    = 1 << 3,
    PDFH_O       = 1 << 4,  // /O and /U: perhaps an encryption dict
    PDFH_U       = 1 << 5,
};

struct PdfFound {
    iz  pos;
    i32 kind;
    i32 num;
    i32 gen;
    i32 flags;
};

// The scan's index: each number's last definition, then what object
// streams hold, and where each stream and trailer ends or begins.
struct PdfIndex {
    PdfSlot  *objs;
    PdfFound *heads;
    iz        nheads;
    iz       *ends;      // ascending
    iz        nends;
    iz       *trailers;  // just past the keyword
    iz        ntrailers;
    b32       cut;       // more were found than PDF_XREFS
    b32       full;      // object streams indexed too
};

// Whether d holds word at i.
static b32 pdfhas(Str d, iz i, Str word)
{
    return i>=0 && d.len-i>=word.len && Str(d.data+i, word.len)==word;
}

// Whether word is at i in d as a keyword, delimited on both sides.
static b32 pdfword(Str d, iz i, Str word)
{
    return pdfhas(d, i, word) && (!i || pdfdelim(d[i-1])) &&
           (i+word.len==d.len || pdfdelim(d[i+word.len]));
}

// Where stream data starts given i just past "stream", or -1 if no end
// of line follows. Writers sometimes leave blanks before the end of line.
static iz pdfstreameol(Str d, iz i)
{
    iz j = i;
    for (; j<d.len && j-i<64 && (d[j]==' ' || d[j]=='\t' || d[j]=='\f' || !d[j]); j++) {}
    if (j<d.len && d[j]=='\r') {
        return j + 1 + (j+1<d.len && d[j+1]=='\n');
    } else if (j<d.len && d[j]=='\n') {
        return j + 1;
    }
    return -1;
}

// A number of at most 10 digits ending just before i, with up to 64
// bytes of whitespace after it, or -1. Its start is put in *at.
static i64 pdfnumbefore(Str d, iz i, iz *at)
{
    iz j = i;
    for (; j>0 && i-j<64 && pdfspace(d[j-1]); j--) {}
    iz end = j;
    for (; j>0 && end-j<10 && digit(d[j-1]); j--) {}
    if (j==end || (j>0 && digit(d[j-1]))) {
        return -1;
    }
    i64 v = 0;
    for (iz k = j; k < end; k++) {
        v = v*10 + d[k] - '0';
    }
    *at = j;
    return v;
}

// Scan the whole file once, charged to the scan budget: find "N G obj"
// headers, noting names their dicts hold, jump over stream data to its
// "endstream" so that what it holds is not taken for objects, and note
// each trailer. Each number's last header is indexed.
static b32 pdfscanfile(Pdf *p)
{
    if (p->scanned1) {
        return p->index != 0;
    }
    p->scanned1 = 1;
    Str d = p->data;
    if (p->why.data) {
        return 0;
    } else if (p->scanned < d.len) {
        p->scanned = 0;
        return pdfstop(p, "it needs more work than is allowed");
    }
    p->scanned -= d.len;

    // Two passes over the same bytes: the first counts, so that the
    // index is sized exactly in keep, and the second fills it in. A
    // growing list would cost several times the input on a file packed
    // with "endstream" and could stop an otherwise good recovery.
    PdfIndex *x = 0;
    for (i32 pass = 0; pass < 2; pass++) {
        iz  cur      = -1;  // the header whose dict is open
        iz  count[3] = {};
        b32 cut      = 0;
        for (iz i = p->head; i<d.len && !p->why.data; i++) {
            u8 c = d[i];
            PdfFound f = {};
            f.pos  = i;
            f.kind = -1;
            if (c=='o' && pdfhas(d, i, "obj") && (i+3==d.len || pdfdelim(d[i+3]))) {
                iz  at  = 0;
                i64 gen = pdfnumbefore(d, i, &at);
                iz  beg = 0;
                i64 num = gen<0 ? -1 : pdfnumbefore(d, at, &beg);
                if (num>0 && num<=PDF_OBJNUM && gen<=0xffff &&
                        (!beg || pdfdelim(d[beg-1]))) {
                    f.kind = PDFX_HEAD;
                    f.pos  = beg;
                    f.num  = (i32)num;
                    f.gen  = (i32)gen;
                }
                i += 2;
            } else if (c=='e' && pdfword(d, i, "endobj")) {
                cur = -1;
                i += 5;
            } else if (c=='e' && pdfhas(d, i, "endstream")) {
                f.kind = PDFX_END;
                i += 8;
            } else if (c=='t' && pdfword(d, i, "trailer")) {
                f.kind = PDFX_TRAILER;
                f.pos  = i + 7;
                cur    = -1;
                i += 6;
            } else if (c=='s' && pdfhas(d, i, "stream") && (!i || pdfdelim(d[i-1])) &&
                       pdfstreameol(d, i+6) >= 0) {
                // To the end of its data: what is found there is the next
                // thing, as if the data were not there
                iz e = find(cuthead(d, i+6), "endstream");
                i = e<0 ? d.len : i+6+e-1;
            } else if (c=='/' && cur>=0 && x) {
                i32 flag = 0;
                if (pdfname(d, i, "/Type") >= 0) {
                    iz at = pdfskip(d, i+5);
                    flag = pdfname(d, at, "/ObjStm")>=0  ? PDFH_OBJSTM
                         : pdfname(d, at, "/Catalog")>=0 ? PDFH_CATALOG
                         : pdfname(d, at, "/Page")>=0    ? PDFH_PAGE
                         : pdfname(d, at, "/XRef")>=0    ? PDFH_XREF
                         : 0;
                } else if (pdfname(d, i, "/O") >= 0) {
                    flag = PDFH_O;
                } else if (pdfname(d, i, "/U") >= 0) {
                    flag = PDFH_U;
                }
                x->heads[cur].flags |= flag;
            }
            if (f.kind < 0) {
                continue;
            }
            cur = f.kind==PDFX_HEAD ? -1 : cur;
            if (count[f.kind] >= PDF_XREFS) {
                cut = 1;
                continue;
            }
            if (f.kind == PDFX_HEAD) {
                cur = count[PDFX_HEAD];
            }
            if (x) {
                switch (f.kind) {
                case PDFX_HEAD:    x->heads[x->nheads++]       = f;     break;
                case PDFX_END:     x->ends[x->nends++]         = f.pos; break;
                case PDFX_TRAILER: x->trailers[x->ntrailers++] = f.pos; break;
                }
            }
            count[f.kind]++;
        }

        if (p->why.data) {
            return 0;
        } else if (!x) {
            x = pdfnew<PdfIndex>(p, &p->keep, 1);
            if (x) {
                x->heads    = pdfnew<PdfFound>(p, &p->keep, count[PDFX_HEAD]);
                x->ends     = pdfnew<iz>(p, &p->keep, count[PDFX_END]);
                x->trailers = pdfnew<iz>(p, &p->keep, count[PDFX_TRAILER]);
                x->cut      = cut;
            }
            if (p->why.data) {
                return 0;
            }
        }
    }

    // The last definition of each number wins
    for (iz i = 0; i<(x ? x->nheads : 0) && !p->why.data; i++) {
        PdfFound f = x->heads[i];
        PdfSlot *s = pdfslot(p, &x->objs, f.num, 1);
        if (s) {
            s->kind = PDFS_AT;
            s->gen  = f.gen;
            s->off  = f.pos;
        }
    }
    if (p->why.data) {
        return 0;
    }
    p->index = x;
    return 1;
}

// The slot an object resolves from: the xref's, unless it has none or
// a free one and the scan found it.
static PdfSlot *pdfslotof(Pdf *p, i32 num)
{
    PdfSlot *s = pdfslot(p, &p->xref, num, 0);
    if ((!s || s->kind==PDFS_FREE) && p->index) {
        PdfSlot *t = pdfslot(p, &p->index->objs, num, 0);
        s = t ? t : s;
    }
    return s;
}

// The first "endstream" at or after i, or the end of the file.
static iz pdfendstream(Pdf *p, iz i)
{
    Str d = p->data;
    if (!pdfscanfile(p)) {
        return d.len;
    }
    PdfIndex *x  = p->index;
    iz        lo = 0;
    iz        hi = x->nends;
    while (lo < hi) {
        iz mid = lo + (hi - lo)/2;
        if (x->ends[mid] < i) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo < x->nends) {
        return x->ends[lo];
    } else if (!x->cut) {
        return d.len;
    }

    // Past what the index could hold: find it, charged
    iz e = find(cuthead(d, i), "endstream");
    iz n = e<0 ? d.len-i : e;
    if (n > p->scanned) {
        p->scanned = 0;
        pdfstop(p, "it needs more work than is allowed");
        return d.len;
    }
    p->scanned -= n;
    return e<0 ? d.len : i+e;
}

// The data of a stream beginning at beg: as long as /Length says if
// that ends before "endstream", else up to the first "endstream" and
// the line end before it, or the end of the file.
static Str pdfextent(Pdf *p, PdfObj dict, iz beg)
{
    Str    d   = p->data;
    PdfObj len = pdfget(p, pdfkey(dict, "Length"));
    i64    n   = len.kind==PDFO_INT ? len.i : -1;
    if (n>=0 && n<=d.len-beg && pdfhas(d, pdfskip(d, beg+(iz)n), "endstream")) {
        return {d.data+beg, (iz)n};
    }
    p->damage++;
    iz e = pdfendstream(p, beg);
    e -= e>beg && d[e-1]=='\n';
    e -= e>beg && d[e-1]=='\r';
    return {d.data+beg, e-beg};
}

// An object's header at i, "N G obj", without allocating.
struct PdfHead {
    i32 num;
    i32 gen;
    iz  end;
    b32 ok;
};

static PdfHead pdfheadat(Pdf *p, iz i)
{
    PdfHead r = {};
    if (i<0 || i>=p->data.len) {
        return r;
    }
    PdfLex x = {p, &p->op, p->data, i, 1, 1};
    PdfTok n = pdflex(&x);
    PdfTok g = pdflex(&x);
    PdfTok o = pdflex(&x);
    if (n.kind==PDFT_INT && n.i>0 && n.i<=PDF_OBJNUM &&
            g.kind==PDFT_INT && g.i>=0 && g.i<=0xffff &&
            o.kind==PDFT_KEY && o.s=="obj") {
        r.num = (i32)n.i;
        r.gen = (i32)g.i;
        r.end = x.i;
        r.ok  = 1;
    }
    return r;
}

// The object whose header ends at i, and its data if it is a stream.
static PdfObj pdfbody(Pdf *p, iz i)
{
    Str    d = p->data;
    PdfLex x = {p, &p->keep, d, i, 1, 0};
    PdfObj o = pdfparse(&x);
    if (o.kind!=PDFO_DICT || pdfpeek(&x)!='s') {
        return o;
    }
    PdfTok t = pdflex(&x);
    if (t.kind!=PDFT_KEY || !(t.s=="stream")) {
        return o;
    }
    iz beg = pdfstreameol(d, x.i);
    beg = beg<0 ? x.i : beg;
    PdfStream *st = pdfnew<PdfStream>(p, &p->keep, 1);
    if (!st) {
        return {};
    }
    st->dict = o;
    st->raw  = pdfextent(p, o, beg);
    PdfObj r = {};
    r.kind = PDFO_STREAM;
    r.st   = st;
    return r;
}

static PdfObj pdfresolve(Pdf *p, PdfSlot *s);
static b32    pdfscanall(Pdf *p);

// An object stream's contents, as its container slot keeps them: what
// /N and /First say, as far as the data bears them out. A container is
// an object at an offset, so resolving one never needs another, and
// /Extends, only a hint, is ignored.
static PdfObjStm *pdfopenstm(Pdf *p, PdfSlot *box)
{
    if (box->stm || box->kind!=PDFS_AT || box->state==1) {
        return box->stm;  // not cached while it resolves: its /Length may be inside
    }
    PdfObjStm *r = pdfnew<PdfObjStm>(p, &p->keep, 1);
    if (!r) {
        return 0;
    }
    box->stm = r;

    PdfObj o = pdfresolve(p, box);
    PdfObj t = pdfkey(o, "Type");
    if (o.kind!=PDFO_STREAM || t.kind!=PDFO_NAME || !(t.s=="ObjStm")) {
        p->damage++;
        return r;
    }
    i64     n     = pdfint(p, o.st->dict, "N", 0);
    i64     first = pdfint(p, o.st->dict, "First", -1);
    PdfData data  = pdfdecode(p, &p->keep, o.st->dict, o.st->raw, PDF_OBJSTM);
    Str     s     = data.image ? Str{} : data.data;
    if (first<0 || first>s.len || n<1) {
        p->damage++;
        return r;
    }
    n = n<s.len/4 ? n : s.len/4;
    r->data = s;
    r->nums = pdfnew<i32>(p, &p->keep, (iz)n);
    r->offs = pdfnew<iz>(p, &p->keep, (iz)n);
    if (!r->offs) {
        return r;
    }
    PdfLex x = {p, &p->op, takehead(s, (iz)first), 0, 1, 1};
    for (i64 k = 0; k < n; k++) {
        PdfTok a = pdflex(&x);
        PdfTok b = pdflex(&x);
        if (a.kind!=PDFT_INT || b.kind!=PDFT_INT) {
            p->damage++;
            break;
        } else if (a.i<1 || a.i>PDF_OBJNUM || b.i<0 || b.i>=s.len-first) {
            p->damage++;
            continue;
        }
        r->nums[r->n] = (i32)a.i;
        r->offs[r->n] = (iz)(first + b.i);
        r->n++;
    }
    return r;
}

// An object at index k of its container, or found among its numbers.
static PdfObj pdfinstm(Pdf *p, PdfSlot *box, i32 num, iz k)
{
    PdfObjStm *m = box ? pdfopenstm(p, box) : 0;
    if (!m || !m->n) {
        return {};
    }
    if (k<0 || k>=m->n || m->nums[k]!=num) {
        if (!pdfcharge(p, m->n)) {
            return {};
        }
        for (k = 0; k<m->n && m->nums[k]!=num; k++) {}
        if (k == m->n) {
            p->damage++;
            return {};
        }
    }
    PdfLex x = {p, &p->keep, m->data, m->offs[k], 1, 0};
    return pdfparse(&x);
}

// Resolve a slot's object, once: kept, even if null. A slot asked for
// while it resolves is null, which breaks cycles such as a stream whose
// /Length is itself. An object not at its offset is looked for where
// the scan found it, at an offset or in an object stream, and if not
// there, the object at the offset is taken for it all the same.
static PdfObj pdfresolve(Pdf *p, PdfSlot *s)
{
    if (!s || s->kind==PDFS_FREE) {
        return {};
    } else if (s->state == 2) {
        return s->val;
    } else if (s->state==1 || p->hops>=PDF_HOPS) {
        p->damage++;
        return {};
    }
    s->state = 1;
    p->hops++;

    PdfObj v = {};
    if (s->kind == PDFS_AT) {
        PdfHead h = pdfheadat(p, s->off);
        if (h.ok && h.num==s->num) {
            p->damage += h.gen != s->gen;
            v = pdfbody(p, h.end);
        } else if (pdfscanall(p)) {
            PdfSlot *t = pdfslot(p, &p->index->objs, s->num, 0);
            PdfHead  g = t && t!=s && t->kind==PDFS_AT ? pdfheadat(p, t->off) : PdfHead{};
            p->damage++;
            if (g.ok && g.num==s->num) {
                v = pdfbody(p, g.end);
            } else if (t && t!=s && t->kind==PDFS_IN) {
                v = pdfinstm(p, t->box, s->num, t->off);
            } else if (h.ok) {
                v = pdfbody(p, h.end);
            }
        }
    } else {
        PdfSlot *box = s->box ? s->box : pdfslotof(p, s->in);
        v = pdfinstm(p, box, s->num, s->off);
    }

    p->hops--;
    s->state = 2;
    s->val   = v;
    return v;
}

// An object by number, null if it is nowhere.
static PdfObj pdfobject(Pdf *p, i32 num)
{
    return num>0 && num<=PDF_OBJNUM ? pdfresolve(p, pdfslotof(p, num)) : PdfObj{};
}

// An object with references resolved, as many as PDF_HOPS deep.
static PdfObj pdfget(Pdf *p, PdfObj o)
{
    for (i32 i = 0; o.kind==PDFO_REF && i<PDF_HOPS; i++) {
        o = pdfobject(p, o.ref.num);
    }
    return o.kind==PDFO_REF ? PdfObj{} : o;
}

// A dict's (or stream's) key's value, both resolved.
static PdfObj pdfget(Pdf *p, PdfObj dict, Str key)
{
    return pdfget(p, pdfkeyc(p, pdfget(p, dict), key));
}

static b32 pdfis(PdfObj o, Str name)
{
    return o.kind==PDFO_NAME && o.s==name;
}

// A stream's data through its filters, in a: see pdfdecode. Not a
// stream gives null.
static PdfData pdfstream(Pdf *p, Arena *a, PdfObj o, iz cap)
{
    o = pdfget(p, o);
    if (o.kind != PDFO_STREAM) {
        return {};
    }
    return pdfdecode(p, a, o.st->dict, o.st->raw, cap);
}


// Reader: xref sections

// Enter an xref entry. Sections come newest first, so the first entry
// for a number wins, except that a free one gives way to one from its
// own section: a hybrid file's table marks free what its xref stream
// (/XRefStm) holds.
static void pdfenter(Pdf *p, i64 num, i32 gen, i32 kind, i64 off, i64 in, i32 rev)
{
    if (num<1 || num>PDF_OBJNUM) {
        return;
    }
    PdfSlot *s = pdfslot(p, &p->xref, (i32)num, 0);
    if (s && !(s->kind==PDFS_FREE && s->rev==rev && kind!=PDFS_FREE)) {
        return;
    } else if (!s && p->entries>=PDF_XREFS) {
        p->damage++;
        return;
    } else if (!s) {
        s = pdfslot(p, &p->xref, (i32)num, 1);
        if (!s) {
            return;
        }
        p->entries++;
    }
    s->gen  = gen;
    s->kind = kind;
    s->rev  = rev;
    s->off  = -1;
    if (kind == PDFS_AT) {
        b32 inside = off>=0 && off<p->data.len-p->base;
        s->off = inside ? p->base+(iz)off : -1;
    } else if (kind == PDFS_IN) {
        s->in  = in>0 && in<=PDF_OBJNUM ? (i32)in : 0;
        s->off = off>=0 && off<=PDF_OBJNUM ? (iz)off : -1;
    }
}

// A classic xref table, its keyword just lexed: subsections of rows,
// each read by its tokens, so that any spacing between them works, up
// to the trailer. Returns the trailer, or null if it was not found.
static PdfObj pdftable(Pdf *p, PdfLex *x, i32 rev)
{
    while (!p->why.data) {
        PdfTok t = pdflex(x);
        if (t.kind==PDFT_KEY && t.s=="trailer") {
            PdfObj r = pdfparse(x);
            return r.kind==PDFO_DICT ? r : PdfObj{};
        }
        PdfTok c = pdflex(x);
        if (t.kind!=PDFT_INT || c.kind!=PDFT_INT || t.i<0 || c.i<0) {
            break;
        }
        for (i64 k = 0; k < c.i; k++) {
            // Rows are read even past the largest number, entering none,
            // and k is bounded by the input, so this cannot overflow
            i64 num = t.i>PDF_OBJNUM ? -1 : t.i+k;
            iz     at   = x->i;
            PdfTok off  = pdflex(x);
            PdfTok gen  = pdflex(x);
            PdfTok type = pdflex(x);
            b32    used = type.kind==PDFT_KEY && type.s=="n";
            b32    free = type.kind==PDFT_KEY && type.s=="f";
            if (off.kind!=PDFT_INT || gen.kind!=PDFT_INT || (!used && !free)) {
                x->i = at;  // a short subsection: on to what follows
                break;
            }
            i32 g = (i32)(gen.i<0 ? 0 : gen.i>0xffff ? 0xffff : gen.i);
            pdfenter(p, num, g, used ? PDFS_AT : PDFS_FREE, off.i, 0, rev);
        }
    }
    p->damage++;
    return {};
}

// A big-endian field of w bytes, at most 8, saturated.
static i64 pdffield(u8 *s, i64 w)
{
    u64 v = 0;
    for (i64 i = 0; i < w; i++) {
        v = v<<8 | s[i];
    }
    return v>(u64)I64MAX ? I64MAX : (i64)v;
}

// An xref stream: rows of three fields of /W widths, in subsections by
// /Index, as many as its data holds.
static void pdfxrefstm(Pdf *p, PdfObj o, i32 rev)
{
    PdfObj d = o.st->dict;
    PdfObj w = pdfget(p, d, "W");
    i64    ws[3] = {};
    i64    rw    = 0;
    for (i32 i = 0; i < 3; i++) {
        PdfObj v = w.kind==PDFO_ARRAY && w.len>=3 ? pdfget(p, w.items[i]) : PdfObj{};
        ws[i] = v.kind==PDFO_INT ? v.i : -1;
        if (ws[i]<0 || ws[i]>8) {
            p->damage++;
            return;
        }
        rw += ws[i];
    }
    if (!rw) {
        p->damage++;
        return;
    }

    // Decoded into page, given back after, as nothing else is there yet
    Arena   save = p->page;
    PdfData data = pdfdecode(p, &p->page, d, o.st->raw, 0);
    Str     s    = data.image ? Str{} : data.data;
    PdfObj  ix   = pdfget(p, d, "Index");
    i64     size = pdfint(p, d, "Size", 0);
    i32     n    = ix.kind==PDFO_ARRAY ? ix.len/2 : 1;
    iz      at   = 0;
    for (i32 i = 0; i<n && !p->why.data; i++) {
        i64 start = 0;
        i64 count = size;
        if (ix.kind == PDFO_ARRAY) {
            PdfObj a = pdfget(p, ix.items[2*i]);
            PdfObj b = pdfget(p, ix.items[2*i+1]);
            start = a.kind==PDFO_INT ? a.i : -1;
            count = b.kind==PDFO_INT ? b.i : 0;
        }
        i64 rows = (s.len - at) / rw;
        rows = count<0 ? 0 : count<rows ? count : rows;
        if (!pdfcharge(p, rows)) {
            break;
        }
        for (i64 k = 0; k < rows; k++, at += (iz)rw) {  // rw at most 24
            u8 *row  = s.data + at;
            i64 type = ws[0] ? pdffield(row, ws[0]) : 1;
            i64 f2   = pdffield(row+ws[0], ws[1]);
            i64 f3   = pdffield(row+ws[0]+ws[1], ws[2]);
            if (start<0 || start>PDF_OBJNUM) {
                continue;  // its rows still take their place
            }
            i64 num = start + k;
            i32 gen = (i32)(f3<0xffff ? f3 : 0xffff);
            switch (type) {
            case 0: pdfenter(p, num, gen, PDFS_FREE, 0, 0, rev);  break;
            case 1: pdfenter(p, num, gen, PDFS_AT, f2, 0, rev);   break;
            case 2: pdfenter(p, num, 0, PDFS_IN, f3, f2, rev);    break;
            }
        }
    }
    p->page = save;
}

// Whether an xref section is at i: the keyword, or an xref stream.
static b32 pdfisxref(Pdf *p, iz i)
{
    if (i<0 || i>=p->data.len) {
        return 0;
    }
    PdfLex x = {p, &p->op, p->data, i, 1, 1};
    PdfTok t = pdflex(&x);
    if (t.kind==PDFT_KEY && t.s=="xref") {
        return 1;
    }
    PdfHead h = pdfheadat(p, i);
    if (!h.ok) {
        return 0;
    }

    // Its dict's /Type, found by walking the top-level keys in skip mode
    // and allocating nothing: startxref may name any object, and parsing
    // a huge one could exhaust memory that recovery then needs.
    PdfLex y = {p, &p->op, p->data, h.end, 1, 1};
    if (pdflex(&y).kind != PDFT_DOPEN) {
        return 0;
    }
    for (;;) {
        PdfTok k = pdflex(&y);
        if (k.kind != PDFT_NAME) {
            return 0;
        }
        PdfTok v = pdflex(&y);
        if (k.s == "Type") {
            return v.kind==PDFT_NAME && v.s=="XRef";
        }
        switch (v.kind) {
        case PDFT_AOPEN:
        case PDFT_DOPEN: pdfskipnest(&y, 1);  break;
        case PDFT_INT:   pdfrefafter(&y, v.i); break;
        case PDFT_REAL:
        case PDFT_STR:
        case PDFT_NAME:
        case PDFT_KEY:   break;
        default:         return 0;
        }
    }
}

// The xref section at i, entered as revision rev. Returns its trailer
// (a stream's dict), null if there is none there.
static PdfObj pdfsection(Pdf *p, iz i, i32 rev)
{
    if (!pdfisxref(p, i)) {
        return {};
    }
    PdfLex x = {p, &p->keep, p->data, i, 1, 0};
    PdfTok t = pdflex(&x);
    if (t.kind==PDFT_KEY && t.s=="xref") {
        return pdftable(p, &x, rev);
    }
    PdfHead h = pdfheadat(p, i);
    PdfObj  o = pdfbody(p, h.end);
    if (o.kind != PDFO_STREAM) {
        p->damage++;
        return {};
    }
    pdfxrefstm(p, o, rev);
    return o.st->dict;
}

// An offset from a trailer, as a position, or -1.
static iz pdfoffset(Pdf *p, PdfObj v)
{
    iz room = p->data.len - p->base;
    return v.kind==PDFO_INT && v.i>=0 && v.i<room ? p->base+(iz)v.i : -1;
}

// Read the xref sections from i back, by /Prev, each with its /XRefStm,
// each once. Returns whether there was one at i.
static b32 pdfchain(Pdf *p, iz i)
{
    Arena mark = pdfmark(p);
    iz   *seen = pdfnew<iz>(p, &p->op, PDF_SECTIONS);
    i32   n    = 0;
    for (i32 rev = 1; seen && i>=0 && n<PDF_SECTIONS && !p->why.data; rev++) {
        for (i32 k = 0; k < n; k++) {
            if (seen[k] == i) {
                p->damage++;  // a loop
                i = -1;
            }
        }
        if (i < 0) {
            break;
        }
        seen[n++] = i;
        PdfObj tr = pdfsection(p, i, rev);
        if (!tr.kind) {
            p->cut |= rev > 1;  // /Prev led nowhere
            break;
        }
        if (!p->trailer.kind) {
            p->trailer = tr;
        }
        p->root      = p->root.kind ? p->root : pdfkey(tr, "Root");
        p->info      = p->info.kind ? p->info : pdfkey(tr, "Info");
        p->encrypted |= pdfkey(tr, "Encrypt").kind != PDFO_NULL;

        PdfObj xv = pdfget(p, pdfkey(tr, "XRefStm"));
        iz     xs = pdfoffset(p, xv);
        if (xs>=0 && n<PDF_SECTIONS) {
            seen[n++] = xs;
            p->cut |= !pdfsection(p, xs, rev).kind;
        } else if (xv.kind) {
            p->cut = 1;
        }
        PdfObj pv = pdfget(p, pdfkey(tr, "Prev"));
        i = pdfoffset(p, pv);
        p->cut |= pv.kind && i<0;
    }
    pdfrewind(p, &mark);
    p->damage += p->cut;
    return n > 0 && p->trailer.kind;
}

// Whether a catalog is usable: a dict with a page tree.
static b32 pdfcatalog(Pdf *p, PdfObj root)
{
    PdfObj c = pdfget(p, root);
    return c.kind==PDFO_DICT && pdfget(p, c, "Pages").kind==PDFO_DICT;
}


// Reader: recovery

// Index what the object streams the scan found hold, newest first, as
// far as no object found at an offset has the number.
static b32 pdfscanall(Pdf *p)
{
    if (!pdfscanfile(p)) {
        return 0;
    }
    PdfIndex *x = p->index;
    if (x->full) {
        return 1;
    }
    x->full = 1;
    for (iz i = x->nheads-1; i>=0 && !p->why.data; i--) {
        PdfFound f = x->heads[i];
        PdfSlot *s = pdfslot(p, &x->objs, f.num, 0);
        if (!(f.flags&PDFH_OBJSTM) || !s || s->off!=f.pos) {
            continue;
        }
        PdfObjStm *m = pdfopenstm(p, s);
        for (i32 k = 0; m && k<m->n && !p->why.data; k++) {
            PdfSlot *t = pdfslot(p, &x->objs, m->nums[k], 1);
            if (t && !t->kind && t->off<0) {
                t->kind = PDFS_IN;
                t->off  = k;
                t->in   = f.num;
                t->box  = s;
            }
        }
    }
    return !p->why.data;
}

static PdfObj pdfnum2ref(i32 num)
{
    PdfObj r = {};
    r.kind    = PDFO_REF;
    r.ref.num = num;
    return r;
}

// What a scanned object stream holds, if it is the last of its number.
static PdfObjStm *pdfcontents(Pdf *p, PdfFound f)
{
    PdfSlot *s = pdfslot(p, &p->index->objs, f.num, 0);
    b32      ok = (f.flags&PDFH_OBJSTM) && s && s->kind==PDFS_AT && s->off==f.pos;
    return ok ? pdfopenstm(p, s) : 0;
}

// A scanned object, if it is the last of its number.
static PdfObj pdffound(Pdf *p, PdfFound f)
{
    PdfSlot *s = pdfslot(p, &p->index->objs, f.num, 0);
    return s && s->kind==PDFS_AT && s->off==f.pos ? pdfresolve(p, s) : PdfObj{};
}

// The structure, found by the scan: the last trailer with a catalog,
// else the last xref stream's dict with one, else the last catalog; and
// whether the file is encrypted, by any trailer or xref stream, or by
// an encryption dict itself (/Filter, /O and /U), which is never
// encrypted, rather than by /Encrypt anywhere, which text may hold.
static void pdfrecover(Pdf *p)
{
    p->recovered = 1;
    p->damage++;
    if (!pdfscanall(p)) {
        return;
    }
    PdfIndex *x    = p->index;
    PdfObj    root = {};
    for (iz i = x->ntrailers-1; i>=0 && !p->why.data; i--) {
        PdfLex t  = {p, &p->keep, p->data, x->trailers[i], 1, 0};
        PdfObj tr = pdfparse(&t);
        p->encrypted |= pdfkey(tr, "Encrypt").kind != PDFO_NULL;
        if (!root.kind && pdfcatalog(p, pdfkey(tr, "Root"))) {
            root       = pdfkey(tr, "Root");
            p->info    = pdfkey(tr, "Info");
            p->trailer = tr;
        }
    }
    for (iz i = x->nheads-1; i>=0 && !p->why.data; i--) {
        PdfFound f = x->heads[i];
        if (f.flags & PDFH_XREF) {
            PdfObj o = pdffound(p, f);
            p->encrypted |= pdfkey(o, "Encrypt").kind != PDFO_NULL;
            if (!root.kind && pdfcatalog(p, pdfkey(o, "Root"))) {
                root       = pdfkey(o, "Root");
                p->info    = pdfkey(o, "Info");
                p->trailer = o.kind==PDFO_STREAM ? o.st->dict : o;
            }
        }
        if ((f.flags&PDFH_O) && (f.flags&PDFH_U)) {
            PdfObj o = pdffound(p, f);
            p->encrypted |= pdfget(p, o, "Filter").kind==PDFO_NAME &&
                            pdfget(p, o, "O").kind==PDFO_STR &&
                            pdfget(p, o, "U").kind==PDFO_STR;
        }
    }
    for (iz i = x->nheads-1; i>=0 && !root.kind && !p->why.data; i--) {
        PdfFound   f = x->heads[i];
        PdfObjStm *m = pdfcontents(p, f);
        for (i32 k = m ? m->n-1 : -1; k>=0 && !root.kind; k--) {
            PdfObj o = pdfobject(p, m->nums[k]);
            if (pdfis(pdfkey(o, "Type"), "Catalog") && pdfcatalog(p, o)) {
                root = pdfnum2ref(m->nums[k]);
            }
        }
        PdfObj o = (f.flags&PDFH_CATALOG) ? pdffound(p, f) : PdfObj{};
        if (!root.kind && pdfis(pdfkey(o, "Type"), "Catalog") && pdfcatalog(p, o)) {
            root = pdfnum2ref(f.num);
        }
    }
    p->root = root;
}

// Load the document: its header, its xref sections from startxref, or
// failing those, its structure from a scan. The offsets in the xref are
// taken to count from the header, as when junk was put before it, or
// failing that from the start of the file.
static i32 pdfload(Pdf *p)
{
    Str d  = p->data;
    iz  at = find(takehead(d, d.len<PDF_HEADSCAN ? d.len : (iz)PDF_HEADSCAN), "%PDF-");
    if (at < 0) {
        return PDFR_NOTPDF;
    }
    p->head = at;

    Str tail = taketail(d, d.len-at<PDF_TAILSCAN ? d.len-at : (iz)PDF_TAILSCAN);
    iz  sx   = -1;
    for (iz i = tail.len-9; i >= 0; i--) {
        if (pdfhas(tail, i, "startxref")) {
            sx = d.len - tail.len + i + 9;
            break;
        }
    }
    b32 loaded = 0;
    if (sx >= 0) {
        PdfLex x = {p, &p->op, d, sx, 1, 1};
        PdfTok t = pdflex(&x);
        if (t.kind==PDFT_INT && t.i>=0 && t.i<d.len) {
            iz off = (iz)t.i;
            p->base = pdfisxref(p, at+off) ? at : pdfisxref(p, off) ? 0 : -1;
            if (p->base >= 0) {
                loaded = pdfchain(p, p->base+off);
            }
            p->base = p->base<0 ? at : p->base;
        }
    }
    if (!loaded || !pdfcatalog(p, p->root)) {
        pdfrecover(p);
    } else if (p->cut) {
        // Objects only older revisions define fall back to the scan
        pdfscanall(p);
    }
    return p->encrypted ? PDFR_ENCRYPTED : PDFR_OK;
}

// Whether data is an encrypted PDF, by loading it alone.
static b32 pdflocked(Arena scratch, Str data)
{
    Pdf p = pdfstart(scratch, data, {});
    return pdfload(&p) == PDFR_ENCRYPTED;
}


// Reader: pages

// A page, with what it inherits from the tree above it, resolved.
struct PdfPage {
    PdfObj page;
    PdfObj resources;
    PdfObj mediabox;
    PdfObj cropbox;
    i32    rotate;  // 0, 90, 180 or 270
    i32    num;     // its object's number, 0 if direct
};

struct PdfPages {
    Slice<PdfPage> pages;
    b32            fallback;  // no tree: the page objects in file order
};

// What a node passes down, overridden by what it has itself.
static PdfPage pdfinherit(Pdf *p, PdfPage up, PdfObj node)
{
    PdfObj v = {};
    if ((v = pdfget(p, node, "Resources")).kind) up.resources = v;
    if ((v = pdfget(p, node, "MediaBox")).kind)  up.mediabox  = v;
    if ((v = pdfget(p, node, "CropBox")).kind)   up.cropbox   = v;
    v = pdfget(p, node, "Rotate");
    if (v.kind==PDFO_INT || v.kind==PDFO_REAL) {
        i64 r = v.kind==PDFO_INT ? v.i : pdftoint(v.r);
        r = (r%360 + 360) % 360;
        up.rotate = (i32)(r - r%90);
    }
    return up;
}

// Whether a walk visits an object for the first time, which marks it.
// Direct objects cannot be reached twice.
static b32 pdfvisit(Pdf *p, PdfObj ref)
{
    if (ref.kind != PDFO_REF) {
        return 1;
    }
    PdfSlot *s = pdfslotof(p, ref.ref.num);
    if (!s) {
        return 1;
    } else if (s->epoch == p->epoch) {
        p->damage++;
        return 0;
    }
    s->epoch = p->epoch;
    return 1;
}

// A page object, unless visited, with what it inherits by its /Parent:
// each from the nearest ancestor that has it. Chain is room for them.
static void pdflonepage(Pdf *p, PdfPages *r, i32 num, PdfObj *chain)
{
    PdfObj ref = pdfnum2ref(num);
    if (r->pages.len>=PDF_PAGEMAX || !pdfvisit(p, ref)) {
        return;
    }
    PdfObj v = pdfget(p, ref);
    if (v.kind!=PDFO_DICT || !pdfis(pdfkey(v, "Type"), "Page")) {
        return;
    }
    PdfPage pg = {};
    i32     n  = 0;
    for (PdfObj a = v; a.kind==PDFO_DICT && n<PDF_TREEDEPTH; n++) {
        chain[n] = a;
        a = pdfget(p, a, "Parent");
    }
    for (i32 k = n-1; k >= 0; k--) {
        pg = pdfinherit(p, pg, chain[k]);
    }
    pg.page  = v;
    pg.num   = num;
    r->pages = pdfpush(p, &p->keep, r->pages, pg);
}

// Every page, in order: the tree walked depth first, without recursion,
// each node once, its /Count ignored. A tree with no pages gives the
// page objects the scan found, in file order, those in object streams
// too, each inheriting by its /Parent.
static PdfPages pdfpages(Pdf *p)
{
    struct Frame {
        PdfObj  kids;
        i32     i;
        PdfPage up;
    };
    PdfPages r     = {};
    Arena    mark  = pdfmark(p);
    Frame   *stack = pdfnew<Frame>(p, &p->op, PDF_TREEDEPTH);
    i32      depth = 0;
    PdfObj   node  = pdfkey(pdfget(p, p->root), "Pages");
    PdfPage  up    = {};
    p->epoch++;
    while (stack && !p->why.data && r.pages.len<PDF_PAGEMAX) {
        PdfObj v = node.kind && pdfvisit(p, node) ? pdfget(p, node) : PdfObj{};
        if (v.kind == PDFO_DICT) {
            PdfPage pg = pdfinherit(p, up, v);
            PdfObj  k  = pdfget(p, v, "Kids");
            if (pdfis(pdfkey(v, "Type"), "Pages") || k.kind==PDFO_ARRAY) {
                if (depth < PDF_TREEDEPTH) {
                    stack[depth].kids = k;
                    stack[depth].i    = 0;
                    stack[depth].up   = pg;
                    depth++;
                } else {
                    p->damage++;
                }
            } else {
                pg.page = v;
                pg.num  = node.kind==PDFO_REF ? node.ref.num : 0;
                r.pages = pdfpush(p, &p->keep, r.pages, pg);
            }
        }

        // The next kid, from the innermost node with one left
        node = {};
        for (; depth>0; depth--) {
            Frame *f = stack + depth - 1;
            if (f->kids.kind==PDFO_ARRAY && f->i<f->kids.len) {
                node = f->kids.items[f->i++];
                up   = f->up;
                break;
            }
        }
        if (!depth || !pdfcharge(p, 1)) {
            break;
        }
    }
    pdfrewind(p, &mark);

    if (r.pages.len || p->why.data || !pdfscanfile(p)) {
        return r;
    }
    r.fallback = 1;
    p->epoch++;
    PdfIndex *x     = p->index;
    PdfObj   *chain = pdfnew<PdfObj>(p, &p->op, PDF_TREEDEPTH);
    for (iz i = 0; chain && i<x->nheads && !p->why.data; i++) {
        PdfFound   f = x->heads[i];
        PdfObjStm *m = pdfcontents(p, f);
        for (i32 k = 0; m && k<m->n && !p->why.data; k++) {
            pdflonepage(p, &r, m->nums[k], chain);
        }
        if (f.flags & PDFH_PAGE) {
            pdflonepage(p, &r, f.num, chain);
        }
    }
    pdfrewind(p, &mark);
    return r;
}

// Whether a document loads only by repair, which a stricter reader may
// refuse: junk before its header, its structure from a scan, an xref
// section or an object that missed, or a page tree that loops, finds no
// pages, or whose /Count is not its pages. For read, which extracts one
// rather than send it as a document, which Anthropic refuses. Loads it
// and walks its page tree, and looks no further: damage inside a page's
// content does not count.
static b32 pdfdamaged(Arena scratch, Str data)
{
    Pdf p = pdfstart(scratch, data, {});
    if (pdfload(&p) != PDFR_OK) {
        return 0;
    } else if (p.head || p.recovered || p.damage) {
        return 1;
    }
    PdfPages g = pdfpages(&p);
    PdfObj   n = pdfget(&p, pdfget(&p, p.root), "Pages");
    n = pdfget(&p, n, "Count");
    return g.fallback || p.recovered || p.damage || p.why.data ||
           n.kind!=PDFO_INT || n.i!=g.pages.len;
}

// An image drawn on a page, as pdfread notes it for pdfimage: an XObject
// by its object, or an inline image by the content stream holding it (a
// page's, stream its index in /Contents, or a form's, stream -1) and the
// offset of its BI there. Its rectangle on the page in default user
// space, the fill colour (for a stencil mask), its size, and a label.
struct PdfPlace {
    i32 id;         // its number in the text: "[image N: ...]"
    i32 page;       // from 1
    iz  line;       // of its placeholder, from 1
    i32 objnum;
    i32 gen;
    i32 stream;
    iz  inlineoff;  // -1 for an XObject
    f32 x0, y0, x1, y1;
    u8  fill[3];
    i32 w, h;
    Str label;      // "480x96 JPEG"
    b32 wholepage;  // covers 80% of the page or more
    i32 image;      // PDFI_*: an image filter left to apply
    b32 mask;       // an ImageMask: a stencil painted in fill
};
