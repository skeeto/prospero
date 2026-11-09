// PDF text: CMaps, fonts, the content interpreter, a first layout of
// glyphs into lines, and pdfread, which reads a whole document's text,
// a "--- page N ---" line starting each page, with a placeholder line
// for each image drawn and where it was drawn (PdfPlace), to be decoded
// later by pdfimage.
//
//   PdfOut r = pdfread(&perm, scratch, data, &req);  // req may be null
//   r.status: PDFR_OK, _PARTIAL (r.why says what cut it short), or a
//             refusal with r.why: _NOTPDF, _ENCRYPTED, _NOPAGES
//   r.text, r.pageline[k]: the 1-based line of page k+1's marker
//
// Like the parser, nothing here calls os_*, every size taken from the
// file goes through the charged wrappers, and every loop over file data
// consumes a byte or charges work. Only results go in perm: the text,
// then the page lines and placements. Fonts, CMaps and image numbers
// last the document, in keep; glyphs, a page's decoded contents and its
// layout last a page, in page, rewound after each; operands last an
// operator, in op.
//
// This is free and unencumbered software released into the public domain.

enum : i32 {
    PDF_CODESPACES = 16,       // codespace ranges in one CMap
    PDF_MAPMAX     = 64,       // code points one code maps to
    PDF_MCDEPTH    = 64,       // nested marked content tracked
    PDF_FONTCODES  = 256,      // codes of a simple font
    PDF_NOTEROOM   = 1 << 10,  // text kept back under PDF_TEXTMAX for notes
    PDF_GROUPS     = 8,        // writing directions told apart on a page
    PDF_GAPS       = 32,       // free intervals of one line, for gutters
    PDF_SIZES      = 32,       // line sizes told apart, for the body's
    PDF_COLUMNS    = 8,        // gutters in one band
    PDF_GUTLINES   = 3,        // lines on each side that make a gutter
    PDF_BOLDWIN    = 8,        // units before one compared for fake bold
    PDF_PLACESCAN  = 16,       // images passed over per line, by overlap
    PDF_LINEWRAP   = READ_LINEMAX / 2,    // a line wraps at a space past this...
    PDF_LINEHARD   = READ_LINEMAX - 512,  // ...and anywhere past this
    PDF_PIECEMAX   = 480,      // bytes the writer is given at once
    PDF_TITLEMAX   = 200,      // bytes of a title or outline entry
    PDF_FIELDMAX   = 400,      // bytes of a field's value
    PDF_NOTEMAX    = 4096,     // bytes of an annotation's text
    PDF_ANNOTS     = 1000,     // annotations read per page
    PDF_FIELDDEPTH = 16,       // a field's /Parent chain
    PDF_OUTDEPTH   = 16,       // outline levels
};

// Layout thresholds, in ems
static f32 const PDF_SPACEEM   = 0.15f;  // a gap wider than this is a space...
static f32 const PDF_CJKEM     = 0.5f;   // ...between CJK characters this...
static f32 const PDF_WIDEEM    = 1.5f;   // ...and two spaces past this
static f32 const PDF_TRACKQ    = 0.25f;  // the quantile of letter gaps taken...
static f32 const PDF_TRACKMAX  = 0.6f;   // ...as letter spacing, at most this
static f32 const PDF_OVERLAP   = 0.5f;   // of the smaller height, to join a line
static f32 const PDF_TANGLE    = 0.3f;   // letters overlapping by this...
static f32 const PDF_BASELINE  = 0.4f;   // ...on baselines this far apart
static f32 const PDF_BOLDEM    = 0.1f;   // a copy this near is fake bold
static f32 const PDF_GUTTEREM  = 0.7f;   // a gutter is at least this wide
static f32 const PDF_PROSE     = 2.5f;   // words per cell in a column of prose
static f32 const PDF_PARAEM    = 1.6f;   // a step this big starts a paragraph...
static f32 const PDF_PARASTEP  = 1.4f;   // ...or this many median steps...
static f32 const PDF_PARAMIN   = 1.25f;  // ...but never less than this
static f32 const PDF_CROPSLACK = 0.1f;   // of the page, around it, still on it

static_assert(PDF_LINEHARD + PDF_PIECEMAX + 2 < READ_LINEMAX);


// Numbers and matrices

// Whether v is a number a page may use: finite, at most 1e15.
static b32 pdfsane(f64 v)
{
    return v>=-1e15 && v<=1e15;  // false for NaN
}

// A square root, without libm: an estimate from the exponent, refined
// by Newton's method. Zero for anything not positive and finite.
static f64 pdfsqrt(f64 x)
{
    if (!(x>0 && x<1e300)) {
        return x>=1e300 ? 1e150 : 0;
    }
    u64 b = 0;
    __builtin_memcpy(&b, &x, sizeof(b));
    b = (b>>1) + ((u64)0x3ff << 51);
    f64 g = 0;
    __builtin_memcpy(&g, &b, sizeof(g));
    for (i32 i = 0; i < 6; i++) {
        g = 0.5*(g + x/g);
    }
    return g;
}

static f64 pdflen(f64 x, f64 y)
{
    return pdfsqrt(x*x + y*y);
}

static f64 pdfabs(f64 v)
{
    return v<0 ? -v : v;
}

// A float as a clamped integer, NaN as zero.
static i32 pdftoi32(f64 v)
{
    return v!=v ? 0 : v>=2e9 ? 2000000000 : v<=-2e9 ? -2000000000 : (i32)v;
}

struct PdfMat {
    f64 a, b, c, d, e, f;
};

static PdfMat const PDF_IDENTITY = {1, 0, 0, 1, 0, 0};

// m then n, as PDF composes them: a point is transformed by m first.
static PdfMat pdfmul(PdfMat m, PdfMat n)
{
    PdfMat r = {};
    r.a = m.a*n.a + m.b*n.c;
    r.b = m.a*n.b + m.b*n.d;
    r.c = m.c*n.a + m.d*n.c;
    r.d = m.c*n.b + m.d*n.d;
    r.e = m.e*n.a + m.f*n.c + n.e;
    r.f = m.e*n.b + m.f*n.d + n.f;
    return r;
}

static b32 pdfsanemat(PdfMat m)
{
    return pdfsane(m.a) && pdfsane(m.b) && pdfsane(m.c) &&
           pdfsane(m.d) && pdfsane(m.e) && pdfsane(m.f);
}

// A number object's value, or def.
static f64 pdfreal(PdfObj o, f64 def = 0)
{
    switch (o.kind) {
    case PDFO_INT:  return (f64)o.i;
    case PDFO_REAL: return o.r;
    }
    return def;
}

// A matrix from six numbers, or the identity if they are not that.
static PdfMat pdfmatof(Pdf *p, PdfObj a, b32 *ok)
{
    a = pdfget(p, a);
    *ok = 0;
    if (a.kind!=PDFO_ARRAY || a.len!=6) {
        return PDF_IDENTITY;
    }
    f64 v[6] = {};
    for (i32 i = 0; i < 6; i++) {
        PdfObj o = pdfget(p, a.items[i]);
        if (o.kind!=PDFO_INT && o.kind!=PDFO_REAL) {
            return PDF_IDENTITY;
        }
        v[i] = pdfreal(o);
    }
    PdfMat m = {v[0], v[1], v[2], v[3], v[4], v[5]};
    *ok = pdfsanemat(m);
    return *ok ? m : PDF_IDENTITY;
}


// CMaps
//
// A CMap maps codes of one to four bytes to CIDs (an encoding) or to
// Unicode (ToUnicode), as ranges sorted by (bytes, CID or not, first
// code), never expanded, so a range over every code costs one entry.
// Ranges that overlap are cut apart when parsed, the later winning, so
// that a lookup is one binary search. A range maps
// to a first CID, or to code points whose last is incremented along the
// range, or to an array of destinations, one per code. Codespace ranges
// say how a font's strings split into codes.

enum : u8 {
    PDFM_CID,  // dst: the first code's CID
    PDFM_UNI,  // dst: pool index of len code points, the last incremented
    PDFM_ARR,  // dst: pool index of an index per code: [len, cp...] each
};

struct PdfSpace {
    i32 n;
    u8  lo[4];
    u8  hi[4];
};

struct PdfRange {
    u32 lo;
    u32 hi;
    u32 base;  // the code dst is for: lo, unless cut from a wider range
    u8  n;
    u8  kind;
    u16 len;
    u32 dst;
};

struct PdfCMap {
    PdfSpace  space[PDF_CODESPACES];
    i32       nspace;
    PdfRange *r;
    iz        nr;
    u32      *pool;
    iz        npool;
    b32       vertical;
    b32       identity;  // two-byte codes that are their CIDs
    b32       utf16;     // codes are UTF-16BE, as in Uni*-UCS2-H
    b32       legacy;    // an unknown predefined CMap: high bytes lead pairs
};

// Whether a range sorts before another: by bytes, CID or not, then
// first code.
static b32 pdfrangeless(PdfRange *a, PdfRange *b)
{
    b32 ca = a->kind==PDFM_CID;
    b32 cb = b->kind==PDFM_CID;
    return a->n!=b->n ? a->n<b->n : ca!=cb ? ca<cb : a->lo<b->lo;
}

static u32 pdfcode(Str s)  // big endian, at most 4 bytes
{
    u32 c = 0;
    for (iz i = 0; i < s.len; i++) {
        c = c<<8 | s[i];
    }
    return c;
}

// A destination string's code points (UTF-16BE), at most PDF_MAPMAX,
// into out. An empty one maps nothing, and so does <0000> for one code,
// though a range may start at it.
static i32 pdfdstcps(Str s, u32 *out, b32 range = 0)
{
    i32 n = 0;
    for (iz i = 0; i<s.len && n<PDF_MAPMAX;) {
        out[n++] = (u32)pdfutf16(s, &i, 0);
    }
    return n==1 && !out[0] && !range ? 0 : n;
}

struct PdfCMapBuild {
    Slice<PdfRange> r;
    Slice<u32>      pool;
};

static void pdfcmapput(Pdf *p, PdfCMapBuild *b, PdfRange r)
{
    if (b->r.len >= PDF_CMAPRANGES) {
        p->damage++;
        return;
    }
    b->r = pdfpush(p, &p->op, b->r, r);
}

// Append code points to the pool, returning where they start, or -1.
static iz pdfcmappool(Pdf *p, PdfCMapBuild *b, u32 *cps, i32 n, b32 counted)
{
    iz at = b->pool.len;
    if (counted) {
        b->pool = pdfpush(p, &p->op, b->pool, (u32)n);
    }
    for (i32 i = 0; i < n; i++) {
        b->pool = pdfpush(p, &p->op, b->pool, cps[i]);
    }
    return p->why.data || at>0x7fffffff ? -1 : at;
}

// One codespace, bfchar, bfrange, cidchar or cidrange entry.
static void pdfcmapentry(Pdf *p, PdfCMap *m, PdfCMapBuild *b, Str mode, PdfObj *a)
{
    if (a[0].kind!=PDFO_STR || a[0].s.len<1 || a[0].s.len>4) {
        p->damage++;
        return;
    }
    Str lo = a[0].s;
    i32 n  = (i32)lo.len;
    u32 cps[PDF_MAPMAX];

    if (mode == "codespacerange") {
        if (a[1].kind!=PDFO_STR || a[1].s.len!=n || m->nspace>=PDF_CODESPACES) {
            p->damage++;
            return;
        }
        PdfSpace *s = m->space + m->nspace++;
        s->n = n;
        for (i32 i = 0; i < n; i++) {
            s->lo[i] = lo[i];
            s->hi[i] = a[1].s[i];
        }
        return;
    }

    PdfRange r = {};
    r.n  = (u8)n;
    r.lo = r.hi = r.base = pdfcode(lo);
    PdfObj dst = a[1];
    if (mode=="bfrange" || mode=="cidrange") {
        if (a[1].kind!=PDFO_STR || a[1].s.len!=n || pdfcode(a[1].s)<r.lo) {
            p->damage++;
            return;
        }
        r.hi = pdfcode(a[1].s);
        dst  = a[2];
    }

    if (mode=="cidchar" || mode=="cidrange") {
        if (dst.kind!=PDFO_INT || dst.i<0 || dst.i>0xffff) {
            p->damage++;
            return;
        }
        r.kind = PDFM_CID;
        r.dst  = (u32)dst.i;
        pdfcmapput(p, b, r);
        return;
    }

    i32 k = 0;
    switch (dst.kind) {
    case PDFO_STR:
        k = pdfdstcps(dst.s, cps, r.hi>r.lo);
        break;
    case PDFO_NAME:
        k = pdfglyphname(dst.s, cps);
        break;
    case PDFO_ARRAY:
        if (mode == "bfrange") {
            // One destination per code, as many as the array holds
            i64 count = (i64)r.hi - r.lo + 1;
            count = count<dst.len ? count : dst.len;
            if (count < 1) {
                return;
            }
            r.hi = r.lo + (u32)(count - 1);
            iz idx = b->pool.len;
            for (i64 i = 0; i < count; i++) {
                b->pool = pdfpush(p, &p->op, b->pool, (u32)0);
            }
            for (i64 i = 0; i<count && !p->why.data; i++) {
                PdfObj e  = dst.items[i];
                i32    ek = e.kind==PDFO_STR ? pdfdstcps(e.s, cps) : 0;
                iz     at = pdfcmappool(p, b, cps, ek, 1);
                if (at < 0) {
                    return;
                }
                b->pool.data[idx+i] = (u32)at;
            }
            if (p->why.data || idx>0x7fffffff) {
                return;
            }
            r.kind = PDFM_ARR;
            r.dst  = (u32)idx;
            pdfcmapput(p, b, r);
            return;
        }
        break;
    }
    if (!k) {
        return;  // maps nothing: other mappings may
    }
    iz at = pdfcmappool(p, b, cps, k, 0);
    if (at < 0) {
        return;
    }
    r.kind = PDFM_UNI;
    r.len  = (u16)k;
    r.dst  = (u32)at;
    pdfcmapput(p, b, r);
}

// Make m the predefined CMap named name, if it is one that needs no
// tables: Identity-H/V, the Uni*-UCS2 and Uni*-UTF16 encodings, and
// any other as legacy, its codes unmapped. Returns whether it knew it.
static b32 pdfcmapnamed(PdfCMap *m, Str name)
{
    m->vertical = endswith(name, "-V");
    if (name=="Identity-H" || name=="Identity-V") {
        m->identity = 1;
        return 1;
    } else if (startswith(name, "Uni") &&
               (find(name, "-UCS2-")>=0 || find(name, "-UTF16-")>=0)) {
        m->utf16 = 1;
        return 1;
    }
    m->legacy = 1;
    return 0;
}

static b32 pdfcmapgroup(PdfRange *a, PdfRange *b)
{
    return a->n==b->n && (a->kind==PDFM_CID)==(b->kind==PDFM_CID);
}

// Sorted ranges cut apart where they overlap, into keep, their count
// in *n, or null. Within a group, a range starting later, or at the
// same code but later in order, wins where it reaches: the sweep keeps
// a stack of those it is inside of, the innermost on top, each with
// the first code not yet given out. A range adds at most two pieces.
static PdfRange *pdfcmapcut(Pdf *p, PdfRange *r, iz *n)
{
    iz        nr  = *n;
    PdfRange *out = pdfnew<PdfRange>(p, &p->keep, 2*nr);
    i32      *in  = pdfnew<i32>(p, &p->op, nr);  // the stack: ranges...
    u64      *cur = pdfnew<u64>(p, &p->op, nr);  // ...and their next codes
    if (!out || !in || !cur || !pdfcharge(p, 4*nr)) {
        return 0;
    }
    iz len = 0;
    iz top = 0;
    auto give = [&](iz k, u64 end) {  // the stack's kth through end
        if (cur[k] <= end) {
            PdfRange e = r[in[k]];
            e.lo = (u32)cur[k];
            e.hi = (u32)end;
            out[len++] = e;
            cur[k] = end + 1;
        }
    };
    for (iz i = 0; i <= nr; i++) {
        b32 fresh = i==nr || (i && !pdfcmapgroup(r+i-1, r+i));
        u64 at    = fresh ? (u64)1<<32 : r[i].lo;
        while (top) {
            u64 hi = r[in[top-1]].hi;
            if (hi < at) {
                give(top-1, hi);
                top--;
                if (top && cur[top-1] <= hi) {
                    cur[top-1] = hi + 1;
                }
            } else {
                if (at) {
                    give(top-1, at - 1);
                }
                break;
            }
        }
        if (i < nr) {
            in[top]  = (i32)i;
            cur[top] = r[i].lo;
            top++;
        }
    }
    *n = len;
    return out;
}

// Parse a CMap's data into m, which may hold a predefined base already:
// codespaces, bf and cid ranges and chars, WMode, and usecmap of an
// Identity. Its entries go in keep, its temporaries in op.
static void pdfcmap(Pdf *p, PdfCMap *m, Str data)
{
    Arena        mark  = pdfmark(p);
    PdfCMapBuild b     = {};
    PdfLex       x     = {p, &p->op, data, 0, 0, 0};
    Str          mode  = {};
    PdfObj       a[3]  = {};
    i32          na    = 0;
    PdfTok       prev  = {};  // the token before, outside of entries
    PdfTok       prev2 = {};
    for (;;) {
        PdfTok t = pdflex(&x);
        if (!t.kind) {
            break;
        }
        if (t.kind == PDFT_KEY) {
            Str s = t.s;
            na = 0;
            if (startswith(s, "begin") && (s=="begincodespacerange" ||
                s=="beginbfchar" || s=="beginbfrange" ||
                s=="begincidchar" || s=="begincidrange")) {
                mode = cuthead(s, 5);
            } else if (startswith(s, "end")) {
                mode = {};
            } else if (s=="usecmap" && prev.kind==PDFT_NAME) {
                if (prev.s=="Identity-H" || prev.s=="Identity-V") {
                    pdfcmapnamed(m, prev.s);
                }
            } else if (s=="def" && prev.kind==PDFT_INT &&
                       prev2.kind==PDFT_NAME && prev2.s=="WMode") {
                m->vertical = prev.i == 1;
            }
            prev2 = prev;
            prev  = t;
            continue;
        }
        if (!mode.len) {
            prev2 = prev;
            prev  = t;
            continue;
        }
        PdfObj o = pdfparsetok(&x, t, 0);
        a[na++] = o;
        i32 need = mode=="codespacerange" || mode=="bfchar" || mode=="cidchar" ? 2 : 3;
        if (na == need) {
            pdfcmapentry(p, m, &b, mode, a);
            na = 0;
        }
    }

    // Kept: the old entries and the new, sorted, the old first so that
    // the new win among equals
    iz        nr   = m->nr + b.r.len;
    iz        np   = m->npool + b.pool.len;
    PdfRange *r    = pdfnew<PdfRange>(p, &p->keep, nr);
    u32      *pool = pdfnew<u32>(p, &p->keep, np);
    if (r && pool) {
        for (iz i = 0; i < m->nr; i++) {
            r[i] = m->r[i];
        }
        for (iz i = 0; i < b.r.len; i++) {
            PdfRange e = b.r.data[i];
            e.dst += e.kind==PDFM_CID ? 0 : (u32)m->npool;
            r[m->nr+i] = e;
        }
        for (iz i = 0; i < m->npool; i++) {
            pool[i] = m->pool[i];
        }
        for (iz i = 0; i < b.pool.len; i++) {
            u32 v = b.pool.data[i];
            pool[m->npool+i] = v;
        }
        // Array indexes are pool positions too
        for (iz i = 0; i < b.r.len; i++) {
            PdfRange e = r[m->nr+i];
            if (e.kind == PDFM_ARR) {
                for (u32 c = 0; c <= e.hi-e.lo; c++) {
                    pool[e.dst+c] += (u32)m->npool;
                }
            }
        }
        pdfrewind(p, &mark);  // the entries parsed are copied: room to sort
        PdfRange *flat = 0;
        if (pdfsort(p, &p->op, r, nr, pdfrangeless)) {
            flat = pdfcmapcut(p, r, &nr);
        }
        if (flat) {
            m->r     = flat;
            m->nr    = nr;
            m->pool  = pool;
            m->npool = np;
        }
    }
    pdfrewind(p, &mark);
}

// The range of m holding code of n bytes, of a CID kind or not, or null:
// the last starting at or before code, by binary search, as none overlap.
static PdfRange *pdfcmapfind(PdfCMap *m, u32 code, i32 n, b32 cid)
{
    PdfRange key = {};
    key.n    = (u8)n;
    key.kind = cid ? PDFM_CID : PDFM_UNI;
    key.lo   = code;
    iz lo = 0;
    iz hi = m->nr;
    while (lo < hi) {
        iz mid = lo + (hi - lo)/2;
        if (!pdfrangeless(&key, m->r+mid)) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    PdfRange *r = lo ? m->r+lo-1 : 0;
    return r && pdfcmapgroup(r, &key) && code<=r->hi ? r : 0;
}

// The code points a ToUnicode CMap gives a code, into out (PDF_MAPMAX),
// or 0 if it has none. A code of other length than the CMap's is tried
// at the lengths it has, as some writers pad simple fonts' codes.
static i32 pdftounicode(PdfCMap *m, u32 code, i32 n, u32 *out)
{
    PdfRange *r = pdfcmapfind(m, code, n, 0);
    for (i32 k = 1; !r && k<=4; k++) {
        r = k!=n && code<((u64)1<<(8*k)) ? pdfcmapfind(m, code, k, 0) : 0;
    }
    if (!r) {
        return 0;
    }
    u32 d = code - r->base;
    if (r->kind == PDFM_UNI) {
        for (i32 i = 0; i < r->len; i++) {
            out[i] = m->pool[r->dst+i];
        }
        out[r->len-1] += d;
        return r->len;
    }
    u32 at  = m->pool[r->dst+d];
    i32 len = (i32)m->pool[at];
    for (i32 i = 0; i < len; i++) {
        out[i] = m->pool[at+1+(u32)i];
    }
    return len;
}

// A code's CID by an encoding CMap.
static u32 pdfcid(PdfCMap *m, u32 code, i32 n)
{
    if (m->utf16 || m->legacy) {
        return code;
    }
    PdfRange *r = m->nr ? pdfcmapfind(m, code, n, 1) : 0;
    return r ? r->dst + (code - r->base) : m->identity || !m->nr ? code : 0;
}

// The next code of a string by m's codespaces, its length in *n: the
// shortest that matches; else one byte. Without codespaces, codes are
// two bytes (Identity), UTF-16 units (Uni*), or a byte, two if high.
static u32 pdfnextcode(PdfCMap *m, Str s, iz *i, i32 *n)
{
    iz left = s.len - *i;
    u8 *c   = s.data + *i;
    if (m->nspace) {
        for (i32 len = 1; len<=4 && len<=left; len++) {
            for (i32 k = 0; k < m->nspace; k++) {
                PdfSpace *sp = m->space + k;
                b32       in = sp->n == len;
                for (i32 j = 0; in && j<len; j++) {
                    in = c[j]>=sp->lo[j] && c[j]<=sp->hi[j];
                }
                if (in) {
                    *n  = len;
                    *i += len;
                    return pdfcode({c, len});
                }
            }
        }
        *n = 1;
        *i += 1;
        return c[0];
    }
    i32 len = 2;
    if (m->legacy) {
        len = c[0]<0x80 ? 1 : 2;
    } else if (m->utf16 && left>=4 && c[0]>=0xd8 && c[0]<=0xdb) {
        len = 4;
    }
    len = len<=left ? len : 1;
    *n  = len;
    *i += len;
    return pdfcode({c, len});
}


// Fonts
//
// A font maps a string's codes to text and advances. Simple fonts take
// a base encoding (the named one, else a standard font's own, else an
// embedded Type 1's from its cleartext, else Standard), then the
// /Differences, with ToUnicode on top of both. Type 0 fonts split codes
// by their CMap and take text from ToUnicode, or from the codes for the
// Uni* CMaps, else nothing: U+FFFD. Widths come from the font, or for a
// standard font without them its own, else 500. Fonts are kept in keep
// by their object, at most PDF_FONTS; past that, a font not kept yet
// reads as the default one.

enum : i32 {
    PDFK_SIMPLE,
    PDFK_TYPE3,
    PDFK_TYPE0,
};

enum : u32 { PDF_MULTI = 0x80000000 };  // uni[] entry: pool index of [n, cp...]

struct PdfWidth {
    u32 lo;
    u32 hi;
    f32 w;    // the width of each, or...
    i32 arr;  // ...index of the first of hi-lo+1 in wpool, if >= 0
};

struct PdfFont {
    i32       kind;
    b32       vertical;
    b32       nomap;      // Type 0 with no way to Unicode: its glyphs counted
                          // unmapped, as a simple font's are one by one
    f32       em;         // an em in text space, per unit of size
    f32       vadv;       // vertical advance per unit of size (negative)
    u32       uni[PDF_FONTCODES];    // simple: code to code point
    f32       width[PDF_FONTCODES];  // simple: advance per unit of size
    u32      *upool;      // simple: multi-code-point entries
    PdfCMap   enc;        // Type 0: codes and CIDs
    PdfCMap   tou;        // ToUnicode
    PdfWidth *w;          // Type 0: widths by CID, sorted
    iz        nw;
    f32      *wpool;
    f32       dw;
};

static b32 pdfwidthless(PdfWidth *a, PdfWidth *b)
{
    return a->lo < b->lo;
}

static b32 pdff32less(f32 *a, f32 *b)
{
    return *a < *b;
}

// An embedded Type 1 font's own encoding, from the cleartext part of
// its program (before eexec): StandardEncoding, or "dup CODE /NAME put"
// entries. Names go through the glyph list. Returns whether it found an
// encoding, filling uni (and pool entries for multiple code points).
static b32 pdftype1enc(Pdf *p, PdfFont *f, Slice<u32> *pool, Str prog)
{
    if (prog.len>=6 && prog[0]==0x80 && prog[1]==1) {
        prog = cuthead(prog, 6);  // a PFB segment header
    }
    iz eexec = find(prog, "eexec");
    prog = eexec<0 ? prog : takehead(prog, eexec);
    iz at = find(prog, "/Encoding");
    if (at < 0) {
        return 0;
    }
    PdfLex x = {p, &p->op, prog, at+9, 0, 0};
    PdfTok t = pdflex(&x);
    if (t.kind==PDFT_KEY && t.s=="StandardEncoding") {
        for (i32 c = 0; c < PDF_FONTCODES; c++) {
            f->uni[c] = PDF_STDENC[c];
        }
        return 1;
    }
    b32    found = 0;
    PdfTok w[3]  = {};  // the three tokens before
    for (i32 n = 0; t.kind && n<1<<16; n++, t = pdflex(&x)) {
        if (t.kind==PDFT_KEY && (t.s=="readonly" || t.s=="def")) {
            break;
        } else if (t.kind==PDFT_KEY && t.s=="put" && w[0].kind==PDFT_NAME &&
                   w[1].kind==PDFT_INT && w[2].kind==PDFT_KEY && w[2].s=="dup" &&
                   w[1].i>=0 && w[1].i<PDF_FONTCODES) {
            if (!found) {
                for (i32 c = 0; c < PDF_FONTCODES; c++) {
                    f->uni[c] = 0;
                }
                found = 1;
            }
            u32 cps[PDF_UNIMAX];
            i32 k = pdfglyphname(w[0].s, cps);
            i32 c = (i32)w[1].i;
            if (k == 1) {
                f->uni[c] = cps[0];
            } else if (k > 1) {
                f->uni[c] = PDF_MULTI | (u32)pool->len;
                *pool = pdfpush(p, &p->keep, *pool, (u32)k);
                for (i32 i = 0; i < k; i++) {
                    *pool = pdfpush(p, &p->keep, *pool, cps[i]);
                }
            } else {
                f->uni[c] = 0;
            }
        }
        w[2] = w[1];
        w[1] = w[0];
        w[0] = t;
    }
    return found;
}

// Load a CMap stream or name into m.
static void pdfcmapobj(Pdf *p, PdfCMap *m, PdfObj o)
{
    o = pdfget(p, o);
    if (o.kind == PDFO_NAME) {
        pdfcmapnamed(m, o.s);
    } else if (o.kind == PDFO_STREAM) {
        PdfObj use = pdfget(p, o, "UseCMap");
        if (use.kind == PDFO_NAME) {
            pdfcmapnamed(m, use.s);
            m->legacy = 0;
        }
        if (pdfint(p, o, "WMode", 0) == 1) {
            m->vertical = 1;
        }
        // Decoded into page, since the decoders' temporaries and the
        // parse go in op; the map itself is copied to keep
        Arena   page = p->page;
        Arena   mark = pdfmark(p);
        PdfData d    = pdfstream(p, &p->page, o, 1<<24);
        if (d.data.len && !d.image) {
            pdfcmap(p, m, d.data);
        }
        pdfrewind(p, &mark);
        p->page = page;
    }
}

// A simple font's widths per unit of size, from /FirstChar, /Widths and
// /MissingWidth, scaled by scale (1/1000, or a Type 3's FontMatrix);
// those unknown are negative.
static void pdfsimplewidths(Pdf *p, PdfFont *f, PdfObj font, PdfObj fd, f64 scale)
{
    for (i32 c = 0; c < PDF_FONTCODES; c++) {
        f->width[c] = -1;
    }
    PdfObj ws  = pdfget(p, font, "Widths");
    i64    fc  = pdfint(p, font, "FirstChar", 0);
    f64    mw  = pdfreal(pdfget(p, fd, "MissingWidth"));
    b32    any = 0;
    if (ws.kind == PDFO_ARRAY) {
        for (i32 i = 0; i<ws.len && pdfcharge(p, 1); i++) {
            i64 c = fc + i;
            f64 w = pdfreal(pdfget(p, ws.items[i]));
            if (c>=0 && c<PDF_FONTCODES && pdfsane(w)) {
                f->width[c] = (f32)(w*scale);
                any |= w != 0;
            }
        }
    }
    if (!any) {
        for (i32 c = 0; c < PDF_FONTCODES; c++) {
            f->width[c] = -1;
        }
    } else if (mw>0 && pdfsane(mw)) {
        for (i32 c = 0; c < PDF_FONTCODES; c++) {
            f->width[c] = f->width[c]<0 ? (f32)(mw*scale) : f->width[c];
        }
    }
}

// A Type 0 font's widths by CID: /W's "c [w...]" and "c1 c2 w" entries,
// and /DW, per unit of size.
static void pdfcidwidths(Pdf *p, PdfFont *f, PdfObj desc)
{
    f->dw = (f32)(pdfreal(pdfget(p, desc, "DW"), 1000)/1000);
    f->dw = pdfsane(f->dw) ? f->dw : 1;
    PdfObj w2 = pdfget(p, desc, "DW2");
    f->vadv = -1;
    if (w2.kind==PDFO_ARRAY && w2.len==2) {
        f64 v = pdfreal(pdfget(p, w2.items[1]), -1000)/1000;
        f->vadv = pdfsane(v) ? (f32)v : -1;
    }

    PdfObj w = pdfget(p, desc, "W");
    if (w.kind != PDFO_ARRAY) {
        return;
    }
    Arena           mark = pdfmark(p);
    Slice<PdfWidth> rs   = {};
    Slice<f32>      pool = {};
    for (i32 i = 0; i+1<w.len && pdfcharge(p, 1) && rs.len<PDF_CMAPRANGES;) {
        PdfObj c  = pdfget(p, w.items[i]);
        PdfObj n  = pdfget(p, w.items[i+1]);
        i64    lo = pdftoint(pdfreal(c, -1));
        PdfWidth r = {};
        if (n.kind == PDFO_ARRAY) {
            i += 2;
            if (lo<0 || lo>0xffffff || !n.len) continue;
            r.lo  = (u32)lo;
            r.hi  = (u32)(lo + n.len - 1);
            r.arr = (i32)pool.len;
            for (i32 k = 0; k<n.len && pdfcharge(p, 1); k++) {
                f64 v = pdfreal(pdfget(p, n.items[k]))/1000;
                pool = pdfpush(p, &p->op, pool, pdfsane(v) ? (f32)v : 0.f);
            }
        } else if (i+2 < w.len) {
            i64 hi = pdftoint(pdfreal(n, -1));
            f64 v  = pdfreal(pdfget(p, w.items[i+2]))/1000;
            i += 3;
            if (lo<0 || hi<lo || hi>0xffffff) continue;
            r.lo  = (u32)lo;
            r.hi  = (u32)hi;
            r.w   = pdfsane(v) ? (f32)v : 0;
            r.arr = -1;
        } else {
            break;
        }
        rs = pdfpush(p, &p->op, rs, r);
    }
    PdfWidth *kr = pdfnew<PdfWidth>(p, &p->keep, rs.len);
    f32      *kp = pdfnew<f32>(p, &p->keep, pool.len);
    if (kr && kp && pdfsort(p, &p->op, rs.data, rs.len, pdfwidthless)) {
        for (iz i = 0; i < rs.len; i++) kr[i] = rs.data[i];
        for (iz i = 0; i < pool.len; i++) kp[i] = pool.data[i];
        f->w     = kr;
        f->nw    = rs.len;
        f->wpool = kp;
    }
    pdfrewind(p, &mark);
}

// A Type 0 font's width for a CID.
static f32 pdfcidwidth(PdfFont *f, u32 cid)
{
    iz lo = 0;
    iz hi = f->nw;
    while (lo < hi) {
        iz mid = lo + (hi - lo)/2;
        if (f->w[mid].lo <= cid) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    for (iz i = lo-1, k = 0; i>=0 && k<8; i--, k++) {
        PdfWidth *r = f->w + i;
        if (cid <= r->hi) {
            return r->arr<0 ? r->w : f->wpool[r->arr + (i32)(cid - r->lo)];
        }
    }
    return f->dw;
}

// Load a font dictionary into f, which is zeroed.
static void pdfloadfont(Pdf *p, PdfFont *f, PdfObj font)
{
    font = pdfget(p, font);
    PdfObj sub  = pdfget(p, font, "Subtype");
    PdfObj bf   = pdfget(p, font, "BaseFont");
    Str    base = bf.kind==PDFO_NAME ? bf.s : Str{};
    f->em = 1;
    pdfcmapobj(p, &f->tou, pdfkey(font, "ToUnicode"));
    if (f->tou.utf16 || f->tou.identity || f->tou.legacy) {
        f->tou = {};  // a name is no ToUnicode
    }

    if (pdfis(sub, "Type0")) {
        f->kind = PDFK_TYPE0;
        pdfcmapobj(p, &f->enc, pdfkey(font, "Encoding"));
        f->vertical = f->enc.vertical;
        PdfObj ds   = pdfget(p, font, "DescendantFonts");
        PdfObj desc = ds.kind==PDFO_ARRAY && ds.len ? pdfget(p, ds.items[0]) : ds;
        pdfcidwidths(p, f, desc);
        f->nomap = !f->tou.nr && !f->enc.utf16;
        return;
    }

    b32        type3 = pdfis(sub, "Type3");
    PdfObj     fd    = pdfget(p, font, "FontDescriptor");
    PdfObj     enc   = pdfget(p, font, "Encoding");
    u16 const *table = 0;
    PdfObj     diffs = {};
    Slice<u32> pool  = {};
    f64        scale = 0.001;
    f->kind = type3 ? PDFK_TYPE3 : PDFK_SIMPLE;

    if (type3) {
        b32    ok = 0;
        PdfMat m  = pdfmatof(p, pdfkey(font, "FontMatrix"), &ok);
        m = ok ? m : PdfMat{0.001, 0, 0, 0.001, 0, 0};
        scale = m.a;

        // An em from the widths: twice their median, in text space
        PdfObj ws  = pdfget(p, font, "Widths");
        f64    em  = 1000*pdfabs(m.d);
        Arena  mk  = pdfmark(p);
        if (ws.kind == PDFO_ARRAY) {
            f32 *v = pdfnew<f32>(p, &p->op, ws.len);
            iz   n = 0;
            for (i32 i = 0; v && i<ws.len && pdfcharge(p, 1); i++) {
                f64 w = pdfabs(pdfreal(pdfget(p, ws.items[i])));
                if (w>0 && pdfsane(w)) v[n++] = (f32)w;
            }
            if (n && pdfsort(p, &p->op, v, n, pdff32less)) {
                em = 2*v[n/2]*pdfabs(m.a);
            }
        }
        pdfrewind(p, &mk);
        f->em = (f32)(em<0.05 ? 0.05 : em>20 ? 20 : em==em ? em : 1);
    }

    if (enc.kind == PDFO_NAME) {
        table = pdfencoding(enc.s);
        table = table || !(enc.s=="MacExpertEncoding") ? table : PDF_STDENC;
    } else if (enc.kind == PDFO_DICT) {
        PdfObj be = pdfget(p, enc, "BaseEncoding");
        table = be.kind==PDFO_NAME ? pdfencoding(be.s) : 0;
        diffs = pdfget(p, enc, "Differences");
    }

    b32 builtin = 0;
    if (!table && !type3) {
        PdfObj ff = pdfget(p, fd, "FontFile");
        if (pdfbuiltin(base)) {
            table = pdfbuiltin(base);
        } else if (ff.kind == PDFO_STREAM) {
            Arena   page = p->page;  // not op: see pdfcmapobj
            Arena   mark = pdfmark(p);
            i64     len1 = pdfint(p, ff, "Length1", 0);
            iz      cap  = len1>0 && len1<(1<<20) ? (iz)len1 : 1<<20;
            PdfData d    = pdfstream(p, &p->page, ff, cap);
            builtin = d.data.len && !d.image && pdftype1enc(p, f, &pool, d.data);
            pdfrewind(p, &mark);
            p->page = page;
        } else if (pdfis(sub, "TrueType")) {
            table = PDF_WINANSI;
        }
        table = table || builtin ? table : PDF_STDENC;
    }
    for (i32 c = 0; table && c<PDF_FONTCODES; c++) {
        f->uni[c] = table[c];
    }

    if (diffs.kind == PDFO_ARRAY) {
        i64 code = 0;
        for (i32 i = 0; i<diffs.len && pdfcharge(p, 1); i++) {
            PdfObj d = pdfget(p, diffs.items[i]);
            if (d.kind==PDFO_INT || d.kind==PDFO_REAL) {
                code = pdftoint(pdfreal(d));
                continue;
            } else if (d.kind != PDFO_NAME) {
                continue;
            }
            if (code>=0 && code<PDF_FONTCODES) {
                u32 cps[PDF_UNIMAX];
                i32 k = pdfglyphname(d.s, cps);
                u32 v = k ? cps[0] : type3 ? pdfcork(d.s, (i32)code) : 0;
                if (k > 1) {
                    v    = PDF_MULTI | (u32)pool.len;
                    pool = pdfpush(p, &p->keep, pool, (u32)k);
                    for (i32 j = 0; j < k; j++) {
                        pool = pdfpush(p, &p->keep, pool, cps[j]);
                    }
                }
                f->uni[code] = v;
            }
            code++;
        }
    }
    f->upool = pool.data;

    pdfsimplewidths(p, f, font, fd, scale);
    for (i32 c = 0; c < PDF_FONTCODES; c++) {
        if (f->width[c] < 0) {
            i32 w = type3 ? 0 : pdfstdwidth(base, c);
            f->width[c] = w ? (f32)w/1000 : (c==32 ? 0.25f : 0.5f)*f->em;
        }
    }
}

// The fonts and image numbers of a document, as its pages are read.
struct PdfFontSlot {
    PdfFontSlot *child[4];
    u64          key;
    PdfFont     *font;
};

struct PdfImgSlot {
    PdfImgSlot *child[4];
    i32         num;
    i32         id;
};

struct PdfDoc {
    Pdf         *p;
    PdfFontSlot *fonts;
    i32          nfonts;
    PdfFont     *deffont;
    PdfImgSlot  *imgs;
    i32          nextid;   // the last image number given
    Slice<PdfPlace> places;  // every page's, in keep
};

static u64 pdfmix(u64 k)
{
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccdu;
    k ^= k >> 33;
    return k;
}

// The font a /Font resource names: kept by its object, or its dict's
// place if direct.
static PdfFont *pdffont(PdfDoc *d, PdfObj ref)
{
    Pdf   *p   = d->p;
    PdfObj v   = pdfget(p, ref);
    if (v.kind != PDFO_DICT) {
        return d->deffont;
    }
    u64 key = ref.kind==PDFO_REF ? (u64)ref.ref.num<<16 | (u64)ref.ref.gen
                                 : (u64)1<<63 | (u64)(uz)v.items;
    PdfFontSlot **m = &d->fonts;
    for (u64 h = pdfmix(key); *m; h <<= 2) {
        if ((*m)->key == key) {
            return (*m)->font;
        }
        m = &(*m)->child[h>>62];
    }
    if (d->nfonts >= PDF_FONTS) {
        p->damage++;
        return d->deffont;
    }
    PdfFontSlot *s = pdfnew<PdfFontSlot>(p, &p->keep, 1);
    PdfFont     *f = pdfnew<PdfFont>(p, &p->keep, 1);
    if (!s || !f) {
        return d->deffont;
    }
    s->key  = key;
    s->font = f;
    *m = s;
    d->nfonts++;
    pdfloadfont(p, f, ref);
    return f;
}


// Content interpreter
//
// Operators run over a page's contents, and forms' inside them, into
// glyph units: each a glyph's text at its origin, with its advance and
// size, in default user space. Marked content's /ActualText stands for
// the glyphs it covers. Images drawn are noted as placements. Text in
// every render mode counts, invisible OCR layers too.

enum : u16 {
    PDFU_SPACE    = 1 << 0,
    PDFU_UNMAPPED = 1 << 1,
    PDFU_FIELD    = 1 << 2,  // a form field's value, set apart by spaces
    PDFU_MARK     = 1 << 3,  // a combining mark, kept after its base
    PDFU_RTL      = 1 << 4,  // a right-to-left letter
    PDFU_LTR      = 1 << 5,  // a left-to-right letter
    PDFU_DIGIT    = 1 << 6,
    PDFU_NUMSEP   = 1 << 7,  // . , or : which digits keep in a number
    PDFU_CJK      = 1 << 8,
};

struct PdfUnit {
    f32 x, y;    // origin
    f32 dx, dy;  // writing direction, a unit vector
    f32 adv;     // advance along it
    f32 size;    // an em
    f32 u, v;    // in its group's frame, for layout
    i32 seq;
    i32 grp;
    i32 off;     // text in the page's pool
    i32 len;
    u16 flags;
    u8  brk;     // spaces before it in its line: 0, 1 or 2
};

// Whether c is a combining mark, which follows its base: the common
// blocks of them, not every one Unicode has.
static b32 pdfcombining(u32 c)
{
    static u32 const r[][2] = {
        {0x0300, 0x036f}, {0x0483, 0x0489}, {0x0591, 0x05bd}, {0x05bf, 0x05bf},
        {0x05c1, 0x05c2}, {0x05c4, 0x05c5}, {0x05c7, 0x05c7}, {0x0610, 0x061a},
        {0x064b, 0x065f}, {0x0670, 0x0670}, {0x06d6, 0x06dc}, {0x06df, 0x06e4},
        {0x06e7, 0x06e8}, {0x06ea, 0x06ed}, {0x0900, 0x0903}, {0x093a, 0x093c},
        {0x093e, 0x094f}, {0x0951, 0x0957}, {0x0962, 0x0963}, {0x0e31, 0x0e31},
        {0x0e34, 0x0e3a}, {0x0e47, 0x0e4e}, {0x1ab0, 0x1aff}, {0x1dc0, 0x1dff},
        {0x20d0, 0x20ff}, {0x302a, 0x302f}, {0x3099, 0x309a}, {0xfe20, 0xfe2f},
    };
    for (i32 i = 0; i < countof(r); i++) {
        if (c>=r[i][0] && c<=r[i][1]) {
            return 1;
        }
    }
    return 0;
}

// What layout needs to know of a unit's text, by its code points.
static u16 pdfclass(u32 *cps, i32 n)
{
    u32 c = cps[0];
    if ((c>='0' && c<='9') || (c>=0x660 && c<=0x669) || (c>=0x6f0 && c<=0x6f9)) {
        for (i32 i = 1; i < n; i++) {
            u32 d = cps[i];
            if (!((d>='0' && d<='9') || (d>=0x660 && d<=0x669) || (d>=0x6f0 && d<=0x6f9))) {
                return 0;
            }
        }
        return PDFU_DIGIT;
    } else if (c=='.' || c==',' || c==':') {
        return n==1 ? PDFU_NUMSEP : 0;
    } else if (pdfcombining(c)) {
        return PDFU_MARK;
    } else if ((c>=0x0590 && c<=0x08ff) || (c>=0xfb1d && c<=0xfdff) ||
               (c>=0xfe70 && c<=0xfeff) || (c>=0x10800 && c<=0x10fff) ||
               (c>=0x1e800 && c<=0x1efff)) {
        return PDFU_RTL;
    } else if (c>=0x3000 && c<=0x303f) {
        return PDFU_CJK;  // CJK punctuation
    } else if ((c>=0x2e80 && c<=0x9fff) || (c>=0xac00 && c<=0xd7af) ||
               (c>=0xf900 && c<=0xfaff) || (c>=0xff00 && c<=0xffef) ||
               (c>=0x20000 && c<=0x3ffff)) {
        return PDFU_CJK | PDFU_LTR;
    } else if (((c|32)>='a' && (c|32)<='z') ||
               (c>=0xc0 && c<0x2000 && c!=0xd7 && c!=0xf7) ||
               (c>=0x2c00 && c<0x2e00) || (c>=0xa640 && c<0xa800)) {
        return PDFU_LTR;
    }
    return 0;
}

struct PdfGs {
    PdfMat   ctm;
    PdfFont *font;
    f64      fs, tc, tw, th, tl, ts;
    i32      tr;
    u8       fill[3];
};

struct PdfRun {
    PdfDoc         *d;
    Pdf            *p;
    PdfGs           gs;
    PdfGs           stack[PDF_QDEPTH];
    i32             sp;
    i32             qover;   // q past the depth, counted
    i32             qbase;   // the innermost form's sp
    PdfMat          tm;
    PdfMat          tlm;
    i32             forms[PDF_FORMDEPTH];
    i32             nforms;
    i32             mcdepth;
    i32             actual;  // depth of the ActualText in effect, or 0
    Str             atext;   // in abuf
    u8             *abuf;    // PDF_NOTEMAX bytes in page, once a page
    iz              aunits;  // units and pool where it began, to drop to
    iz              apool;
    i64             aunmapped;
    b32             ashown;
    f32             ax, ay, adx, ady, asize, aend;
    Slice<PdfUnit>  units;
    Buf             pool;
    Slice<PdfPlace> places;
    i32             seq;
    i64             unmapped;
    b32             glyphcap;
    b32             placecap;
    b32             cut;     // a content stream was cut at PDF_STREAMMAX
    f64             box[4];
    i32             pageno;
    // The content stream running, for an inline image's place
    i32             objnum;
    i32             gen;
    i32             stream;
    iz             *starts;  // of the page's streams in its contents
    i32            *nums;
    i32            *gens;
    i32             nstreams;
    b32             inform;
};

// Whether a unit at origin x, y can be emitted: room for it, on the page.
static b32 pdfunitok(PdfRun *r, f64 x, f64 y, f64 adv, f64 size)
{
    if (r->units.len >= PDF_GLYPHS) {
        r->glyphcap = 1;
        return 0;
    }
    f64 w = r->box[2] - r->box[0];
    f64 h = r->box[3] - r->box[1];
    return size>=1e-3 && size<=1e5 && pdfsane(x) && pdfsane(y) && pdfsane(adv) &&
           x>=r->box[0]-PDF_CROPSLACK*w && x<=r->box[2]+PDF_CROPSLACK*w &&
           y>=r->box[1]-PDF_CROPSLACK*h && y<=r->box[3]+PDF_CROPSLACK*h;
}

static void pdfunitat(PdfRun *r, iz off, f64 x, f64 y, f64 dx, f64 dy,
                      f64 adv, f64 size, u16 flags);

// Emit a unit of text cps at origin x, y.
static void pdfunit(PdfRun *r, u32 *cps, i32 n, f64 x, f64 y, f64 dx, f64 dy,
                    f64 adv, f64 size, u16 flags)
{
    Pdf *p = r->p;
    if (!n || !pdfunitok(r, x, y, adv, size) || !pdfreserve(p, &r->pool, 12*(iz)n)) {
        return;
    }
    iz off = r->pool.len;
    for (i32 i = 0; i < n; i++) {
        pdfputcp(&r->pool, cps[i]);
    }
    pdfunitat(r, off, x, y, dx, dy, adv, size, flags | pdfclass(cps, n));
}

// Emit a unit of clean text s, set apart as a field's value is.
static void pdfunitstr(PdfRun *r, Str s, f64 x, f64 y, f64 adv, f64 size)
{
    Pdf *p = r->p;
    if (!s.len || !pdfunitok(r, x, y, adv, size) || !pdfreserve(p, &r->pool, s.len)) {
        return;
    }
    iz off = r->pool.len;
    print(&r->pool, s);
    pdfunitat(r, off, x, y, 1, 0, adv, size, PDFU_FIELD|PDFU_LTR);
}

// Emit the unit whose text is in the pool from off.
static void pdfunitat(PdfRun *r, iz off, f64 x, f64 y, f64 dx, f64 dy,
                      f64 adv, f64 size, u16 flags)
{
    Pdf    *p = r->p;
    PdfUnit u = {};
    u.x     = (f32)x;
    u.y     = (f32)y;
    u.dx    = (f32)dx;
    u.dy    = (f32)dy;
    u.adv   = (f32)adv;
    u.size  = (f32)size;
    u.seq   = r->seq++;
    u.off   = (i32)off;
    u.len   = (i32)(r->pool.len - off);
    u.flags = flags;
    if (u.len==1 && r->pool.data[off]==' ') {
        u.flags |= PDFU_SPACE;
    } else if (!u.len) {
        return;
    }
    r->units = pdfpush(p, &p->page, r->units, u);
}

// The code points of one code, into cps (PDF_MAPMAX).
static i32 pdfglyphtext(PdfFont *f, u32 code, i32 n, u32 *cps, u8 *flags)
{
    i32 k = pdftounicode(&f->tou, code, n, cps);
    if (k) {
        return k;
    }
    if (f->kind == PDFK_TYPE0) {
        if (f->enc.utf16) {
            u8  b[4] = {(u8)(code>>24), (u8)(code>>16), (u8)(code>>8), (u8)code};
            iz  i    = 4 - n;
            cps[0] = (u32)pdfutf16({b, 4}, &i, 0);
            return 1;
        }
        *flags |= PDFU_UNMAPPED;
        cps[0] = 0xfffd;
        return 1;
    }
    u32 v = code<PDF_FONTCODES ? f->uni[code] : 0;
    if (v & PDF_MULTI) {
        u32 at = v & ~PDF_MULTI;
        i32 len = (i32)f->upool[at];
        for (i32 i = 0; i < len; i++) {
            cps[i] = f->upool[at+1+(u32)i];
        }
        return len;
    } else if (!v && code=='\t') {
        v = ' ';  // a tab, which the encodings leave out
    } else if (!v) {
        *flags |= PDFU_UNMAPPED;
        v = 0xfffd;
    }
    cps[0] = v;
    return 1;
}

// Show a string: each code a glyph unit, the text matrix advanced.
static void pdfshow(PdfRun *r, Str s)
{
    Pdf     *p  = r->p;
    PdfGs   *g  = &r->gs;
    PdfFont *f  = g->font ? g->font : r->d->deffont;
    PdfCMap  one = {};
    PdfCMap *cm = f->kind==PDFK_TYPE0 ? &f->enc : &one;
    one.nspace  = 1;
    one.space[0] = {1, {0}, {0xff}};
    for (iz i = 0; i<s.len && pdfcharge(p, 1);) {
        i32 n    = 1;
        u32 code = pdfnextcode(cm, s, &i, &n);
        f64 w    = 0;
        if (f->kind == PDFK_TYPE0) {
            w = f->vertical ? f->vadv : pdfcidwidth(f, pdfcid(&f->enc, code, n));
        } else {
            w = f->width[code & 0xff];
        }
        f64 spacing = g->tc + (n==1 && code==32 ? g->tw : 0);

        PdfMat m  = pdfmul(r->tm, g->ctm);
        PdfMat t  = pdfmul(PdfMat{g->fs*g->th, 0, 0, g->fs, 0, g->ts}, m);
        f64    tx = 0;
        f64    ty = 0;
        f64    dx, dy, size, adv;
        if (!f->vertical) {
            // A negative size or scale turns the glyphs, and their way
            f64 sg = g->fs*g->th<0 ? -1 : 1;
            tx   = (w*g->fs + spacing) * g->th;
            dx   = sg * m.a;
            dy   = sg * m.b;
            size = pdflen(t.c, t.d) * f->em;
            adv  = sg * tx * pdflen(m.a, m.b);
        } else {
            f64 sg = g->fs<0 ? -1 : 1;
            ty   = w*g->fs + spacing;
            dx   = -sg * m.c;
            dy   = -sg * m.d;
            size = pdflen(t.a, t.b) * f->em;
            adv  = -sg * ty * pdflen(m.c, m.d);
        }
        f64 dl = pdflen(dx, dy);
        dx = dl>0 ? dx/dl : 1;
        dy = dl>0 ? dy/dl : 0;

        u32 cps[PDF_MAPMAX];
        u8  flags = 0;
        i32 k     = pdfglyphtext(f, code, n, cps, &flags);
        r->unmapped += (flags & PDFU_UNMAPPED) && (f->kind!=PDFK_TYPE0 || f->nomap);
        if (r->actual) {
            // Covered by ActualText, which replaces them at its end,
            // there from where it starts to where it ends
            if (!r->ashown && pdfsane(t.e) && pdfsane(t.f)) {
                r->ashown = 1;
                r->ax     = (f32)t.e;
                r->ay     = (f32)t.f;
                r->adx    = (f32)dx;
                r->ady    = (f32)dy;
                r->asize  = (f32)size;
            }
            if (r->ashown && pdfsane(adv)) {
                f64 u = (t.e - r->ax)*r->adx + (t.f - r->ay)*r->ady + adv;
                r->aend = pdfsane(u) && u>r->aend ? (f32)u : r->aend;
            }
        }
        pdfunit(r, cps, k, t.e, t.f, dx, dy, adv, size, flags);

        r->tm.e += tx*r->tm.a + ty*r->tm.c;
        r->tm.f += tx*r->tm.b + ty*r->tm.d;
    }
}

// A TJ array's numbers move the text matrix, in thousandths of an em.
static void pdfkern(PdfRun *r, f64 n)
{
    PdfGs *g  = &r->gs;
    PdfFont *f = g->font ? g->font : r->d->deffont;
    f64    tx = 0;
    f64    ty = 0;
    if (f->vertical) {
        ty = -n/1000 * g->fs;
    } else {
        tx = -n/1000 * g->fs * g->th;
    }
    if (pdfsane(tx) && pdfsane(ty)) {
        r->tm.e += tx*r->tm.a + ty*r->tm.c;
        r->tm.f += tx*r->tm.b + ty*r->tm.d;
    }
}

static void pdfsettm(PdfRun *r, PdfMat m)
{
    if (pdfsanemat(m)) {
        r->tm = r->tlm = m;
    }
}

static void pdftd(PdfRun *r, f64 tx, f64 ty)
{
    pdfsettm(r, pdfmul(PdfMat{1, 0, 0, 1, tx, ty}, r->tlm));
}

// The image number of an object, given once per document.
static i32 pdfimageid(PdfDoc *d, i32 num)
{
    Pdf *p = d->p;
    if (num <= 0) {
        return ++d->nextid;
    }
    PdfImgSlot **m = &d->imgs;
    for (u64 h = pdfmix((u64)num); *m; h <<= 2) {
        if ((*m)->num == num) {
            return (*m)->id;
        }
        m = &(*m)->child[h>>62];
    }
    PdfImgSlot *s = pdfnew<PdfImgSlot>(p, &p->keep, 1);
    if (!s) {
        return ++d->nextid;
    }
    s->num = num;
    s->id  = ++d->nextid;
    *m = s;
    return s->id;
}

// What an image dict says it is: its last filter as an image kind, and
// a word for its pixels. Inline images abbreviate.
static Str pdfimagekind(Pdf *p, PdfObj dict, i32 *image)
{
    PdfObj f = pdfget(p, dict, "Filter");
    f = f.kind ? f : pdfget(p, dict, "F");
    if (f.kind==PDFO_ARRAY && f.len) {
        f = pdfget(p, f.items[f.len-1]);
    }
    i32 id = f.kind==PDFO_NAME ? pdffilterid(f.s) : PDFF_UNKNOWN;
    *image = PDFI_NONE;
    switch (id) {
    case PDFF_DCT:   *image = PDFI_DCT;   return "JPEG";
    case PDFF_JPX:   *image = PDFI_JPX;   return "JPEG 2000";
    case PDFF_JBIG2: *image = PDFI_JBIG2; return "JBIG2";
    case PDFF_CCITT: *image = PDFI_CCITT; return "CCITT";
    }
    PdfObj im = pdfget(p, dict, "ImageMask");
    im = im.kind ? im : pdfget(p, dict, "IM");
    if (im.kind==PDFO_BOOL && im.b) {
        return "mask";
    }
    PdfObj cs = pdfget(p, dict, "ColorSpace");
    cs = cs.kind ? cs : pdfget(p, dict, "CS");
    if (cs.kind==PDFO_ARRAY && cs.len) {
        PdfObj first = pdfget(p, cs.items[0]);
        if (pdfis(first, "ICCBased") && cs.len>1) {
            i64 n = pdfint(p, pdfget(p, cs.items[1]), "N", 0);
            return n==1 ? Str("gray") : n==3 ? Str("RGB") : n==4 ? Str("CMYK") : "";
        }
        cs = first;
    }
    if (cs.kind != PDFO_NAME) {
        return "";
    }
    Str n = cs.s;
    if (n=="DeviceGray" || n=="CalGray" || n=="G" || n=="Separation" || n=="DeviceN") {
        return "gray";
    } else if (n=="DeviceRGB" || n=="CalRGB" || n=="RGB" || n=="Lab") {
        return "RGB";
    } else if (n=="DeviceCMYK" || n=="CMYK") {
        return "CMYK";
    } else if (n=="Indexed" || n=="I") {
        return "indexed";
    }
    return "";
}

// Note an image drawn in the unit square of the current matrix.
static void pdfplace(PdfRun *r, PdfObj dict, i32 objnum, i32 gen, iz inlineoff)
{
    Pdf    *p = r->p;
    PdfDoc *d = r->d;
    if (r->places.len>=PDF_PLACES || d->places.len+r->places.len>=PDF_PLACESALL) {
        r->placecap = 1;
        return;
    }
    PdfMat m = r->gs.ctm;
    f64 xs[4] = {m.e, m.e+m.a, m.e+m.c, m.e+m.a+m.c};
    f64 ys[4] = {m.f, m.f+m.b, m.f+m.d, m.f+m.b+m.d};
    f64 x0 = xs[0], x1 = xs[0], y0 = ys[0], y1 = ys[0];
    for (i32 i = 1; i < 4; i++) {
        x0 = xs[i]<x0 ? xs[i] : x0;
        x1 = xs[i]>x1 ? xs[i] : x1;
        y0 = ys[i]<y0 ? ys[i] : y0;
        y1 = ys[i]>y1 ? ys[i] : y1;
    }
    if (!pdfsane(x0) || !pdfsane(x1) || !pdfsane(y0) || !pdfsane(y1)) {
        return;
    }

    PdfPlace pl = {};
    i32  image = 0;
    Str  kind  = pdfimagekind(p, dict, &image);
    i64  w     = pdfint(p, dict, "Width", -1);
    i64  h     = pdfint(p, dict, "Height", -1);
    w = w>=0 ? w : pdfint(p, dict, "W", 0);
    h = h>=0 ? h : pdfint(p, dict, "H", 0);
    pl.id        = pdfimageid(d, inlineoff<0 ? objnum : 0);
    pl.page      = r->pageno;
    pl.line      = -1;
    pl.objnum    = objnum;
    pl.gen       = gen;
    pl.stream    = inlineoff<0 ? -1 : r->stream;
    pl.inlineoff = inlineoff;
    pl.x0        = (f32)x0;
    pl.y0        = (f32)y0;
    pl.x1        = (f32)x1;
    pl.y1        = (f32)y1;
    pl.fill[0]   = r->gs.fill[0];
    pl.fill[1]   = r->gs.fill[1];
    pl.fill[2]   = r->gs.fill[2];
    pl.w         = (i32)pdfclamp(w, 0, 1<<30);
    pl.h         = (i32)pdfclamp(h, 0, 1<<30);
    pl.image     = image;
    PdfObj im    = pdfget(p, dict, "ImageMask");
    im = im.kind ? im : pdfget(p, dict, "IM");
    pl.mask      = im.kind==PDFO_BOOL && im.b;

    // The whole page: at least 80% of it covered
    f64 bw = r->box[2] - r->box[0];
    f64 bh = r->box[3] - r->box[1];
    f64 cw = (x1<r->box[2] ? x1 : r->box[2]) - (x0>r->box[0] ? x0 : r->box[0]);
    f64 ch = (y1<r->box[3] ? y1 : r->box[3]) - (y0>r->box[1] ? y0 : r->box[1]);
    pl.wholepage = cw>0 && ch>0 && bw>0 && bh>0 && cw*ch >= 0.8*bw*bh;

    // Its label, "480x96 JPEG", kept for the document
    u8 *buf = pdfnew<u8>(p, &p->keep, 64);
    if (!buf) {
        return;
    }
    Buf b = {};
    b.data = buf;
    b.cap  = 64;
    b.a    = &p->keep;  // never grows: at most 10+1+10+1+9 bytes
    print(&b, (i64)pl.w);
    putbyte(&b, 'x');
    print(&b, (i64)pl.h);
    if (kind.len) {
        putbyte(&b, ' ');
        print(&b, kind);
    }
    pl.label  = finish(&b);
    r->places = pdfpush(p, &p->page, r->places, pl);
}

// Colour operands as RGB: gray, RGB or CMYK by how many there are.
static void pdffill(PdfRun *r, PdfObj *ops, i32 n)
{
    f64 v[4] = {};
    i32 k    = 0;
    for (i32 i = 0; i<n && k<4; i++) {
        if (ops[i].kind==PDFO_INT || ops[i].kind==PDFO_REAL) {
            f64 x = pdfreal(ops[i]);
            v[k++] = x<0 ? 0 : x>1 ? 1 : x==x ? x : 0;
        }
    }
    f64 rgb[3] = {};
    if (k==1 || k==2) {
        rgb[0] = rgb[1] = rgb[2] = v[0];
    } else if (k == 3) {
        rgb[0] = v[0], rgb[1] = v[1], rgb[2] = v[2];
    } else if (k == 4) {
        for (i32 i = 0; i < 3; i++) rgb[i] = (1 - v[i]) * (1 - v[3]);
    } else {
        return;
    }
    for (i32 i = 0; i < 3; i++) {
        r->gs.fill[i] = (u8)(rgb[i]*255 + 0.5);
    }
}

static void pdfrun(PdfRun *r, Str content, PdfObj res);
static Str  pdfstrtext(Pdf *, Arena *, PdfObj, iz);

// Draw an XObject: run a form, or place an image.
static void pdfdo(PdfRun *r, PdfObj res, Str name)
{
    Pdf   *p   = r->p;
    PdfObj ref = pdfkeyc(p, pdfget(p, res, "XObject"), name);
    PdfObj x   = pdfget(p, ref);
    if (x.kind != PDFO_STREAM) {
        return;
    }
    PdfObj sub = pdfget(p, x, "Subtype");
    i32    num = ref.kind==PDFO_REF ? ref.ref.num : 0;
    i32    gen = ref.kind==PDFO_REF ? ref.ref.gen : 0;
    if (pdfis(sub, "Image")) {
        pdfplace(r, x, num, gen, -1);
        return;
    } else if (!pdfis(sub, "Form")) {
        return;
    }

    b32 cycle = r->nforms >= PDF_FORMDEPTH;
    for (i32 i = 0; i < r->nforms; i++) {
        cycle |= num && r->forms[i]==num;
    }
    if (cycle) {
        p->damage++;
        return;
    }
    PdfData d = pdfstream(p, &p->page, x, 0);
    r->cut |= d.cut && !p->why.data;
    if (!d.data.len || d.image) {
        return;
    }

    PdfObj fres = pdfget(p, x, "Resources");
    fres = fres.kind==PDFO_DICT ? fres : res;
    b32    ok   = 0;
    PdfMat fm   = pdfmatof(p, pdfkey(x.st->dict, "Matrix"), &ok);

    // Run in a state of its own: what it leaves is dropped
    PdfGs  gs     = r->gs;
    i32    sp     = r->sp;
    i32    qbase  = r->qbase;
    i32    qover  = r->qover;
    PdfMat tm     = r->tm;
    PdfMat tlm    = r->tlm;
    i32    objnum = r->objnum;
    i32    ogen   = r->gen;
    i32    stream = r->stream;
    b32    inform = r->inform;
    i32    mcdepth = r->mcdepth;
    i32    actual  = r->actual;
    r->forms[r->nforms++] = num;
    r->gs.ctm = pdfmul(fm, r->gs.ctm);
    r->qbase  = r->sp;
    r->qover  = 0;
    r->objnum = num;
    r->gen    = gen;
    r->stream = -1;
    r->inform = 1;
    pdfrun(r, d.data, fres);
    r->nforms--;
    r->gs     = gs;
    r->sp     = sp;
    r->qbase  = qbase;
    r->qover  = qover;
    r->tm     = tm;
    r->tlm    = tlm;
    r->objnum = objnum;
    r->gen    = ogen;
    r->stream = stream;
    r->inform = inform;
    // Marked content a form left open ends with it, its ActualText
    // dropped and the glyphs it covered kept
    r->actual  = r->actual>mcdepth ? actual : r->actual;
    r->mcdepth = mcdepth;
}

// An inline image, its BI just lexed at beg: its dict and data skipped
// (pdfinlineat), and placed.
static void pdfinline(PdfRun *r, PdfLex *x, iz beg)
{
    Pdf      *p  = r->p;
    PdfInline in = pdfinlineat(p, x);
    if (!in.ok) {
        return;
    }

    // Its offset in its own stream
    iz  off = beg;
    i32 si  = r->stream;
    if (!r->inform && r->nstreams) {
        si = 0;
        for (i32 k = 1; k<r->nstreams && r->starts[k]<=beg; k++) {
            si = k;
        }
        off = beg - r->starts[si];
        r->objnum = r->nums[si];
        r->gen    = r->gens[si];
    }
    r->stream = si;
    pdfplace(r, in.dict, r->objnum, r->gen, off);
}

// Run a content stream with its resources.
static void pdfrun(PdfRun *r, Str content, PdfObj res)
{
    Pdf    *p    = r->p;
    PdfDoc *doc  = r->d;
    PdfLex  x    = {p, &p->op, content, 0, 0, 0};
    PdfObj  ops[PDF_OPERANDS];
    i32     n    = 0;
    Arena   mark = pdfmark(p);
    for (;;) {
        PdfTok t = pdflex(&x);
        if (!t.kind) {
            break;
        } else if (t.kind==PDFT_BAD || t.kind==PDFT_BOPEN || t.kind==PDFT_BCLOSE ||
                   t.kind==PDFT_ACLOSE || t.kind==PDFT_DCLOSE) {
            continue;
        } else if (t.kind!=PDFT_KEY || t.s=="true" || t.s=="false" || t.s=="null") {
            if (n == PDF_OPERANDS) {
                n = 0;
                pdfrewind(p, &mark);
            }
            ops[n++] = pdfparsetok(&x, t, 0);
            continue;
        }

        Str     op = t.s;
        PdfGs  *g  = &r->gs;
        f64     v[6] = {};
        for (i32 i = 0; i<n && i<6; i++) {
            v[i] = pdfreal(ops[n-1-i]);  // v[0] the last operand
        }
        b32 nums = 1;
        for (i32 i = 0; i < n; i++) {
            nums &= (ops[i].kind==PDFO_INT || ops[i].kind==PDFO_REAL) && pdfsane(pdfreal(ops[i]));
        }

        switch (op.len) {
        case 1:
            switch (op[0]) {
            case 'q':
                if (r->sp < PDF_QDEPTH) {
                    r->stack[r->sp++] = *g;
                } else {
                    r->qover++;
                }
                break;
            case 'Q':
                if (r->qover) {
                    r->qover--;
                } else if (r->sp > r->qbase) {
                    *g = r->stack[--r->sp];
                }
                break;
            case 'g':
                if (n >= 1) pdffill(r, ops+n-1, 1);
                break;
            case 'k':
                if (n >= 4) pdffill(r, ops+n-4, 4);
                break;
            case '\'':
                pdftd(r, 0, -g->tl);
                if (n>=1 && ops[n-1].kind==PDFO_STR) pdfshow(r, ops[n-1].s);
                break;
            case '"':
                if (n>=3 && ops[n-1].kind==PDFO_STR) {
                    f64 tw = pdfreal(ops[n-3]);
                    f64 tc = pdfreal(ops[n-2]);
                    g->tw = pdfsane(tw) ? tw : g->tw;
                    g->tc = pdfsane(tc) ? tc : g->tc;
                    pdftd(r, 0, -g->tl);
                    pdfshow(r, ops[n-1].s);
                }
                break;
            }
            break;

        case 2:
            if (op=="cm" && n==6 && nums) {
                PdfMat m = {v[5], v[4], v[3], v[2], v[1], v[0]};
                m = pdfmul(m, g->ctm);
                g->ctm = pdfsanemat(m) ? m : PdfMat{};
            } else if (op == "BT") {
                r->tm = r->tlm = PDF_IDENTITY;
            } else if (op=="Tf" && n>=2) {
                PdfObj name = ops[n-2];
                if (name.kind == PDFO_NAME) {
                    g->font = pdffont(doc, pdfkeyc(p, pdfget(p, res, "Font"), name.s));
                }
                g->fs = pdfsane(v[0]) ? v[0] : g->fs;
            } else if (op=="Tc" && n>=1) {
                g->tc = pdfsane(v[0]) ? v[0] : g->tc;
            } else if (op=="Tw" && n>=1) {
                g->tw = pdfsane(v[0]) ? v[0] : g->tw;
            } else if (op=="Tz" && n>=1) {
                g->th = pdfsane(v[0]) ? v[0]/100 : g->th;
            } else if (op=="TL" && n>=1) {
                g->tl = pdfsane(v[0]) ? v[0] : g->tl;
            } else if (op=="Ts" && n>=1) {
                g->ts = pdfsane(v[0]) ? v[0] : g->ts;
            } else if (op=="Tr" && n>=1) {
                g->tr = pdftoi32(v[0]);
            } else if (op=="Td" && n>=2 && pdfsane(v[0]) && pdfsane(v[1])) {
                pdftd(r, v[1], v[0]);
            } else if (op=="TD" && n>=2 && pdfsane(v[0]) && pdfsane(v[1])) {
                g->tl = -v[0];
                pdftd(r, v[1], v[0]);
            } else if (op=="Tm" && n>=6 && nums) {
                pdfsettm(r, PdfMat{v[5], v[4], v[3], v[2], v[1], v[0]});
            } else if (op == "T*") {
                pdftd(r, 0, -g->tl);
            } else if (op=="Tj" && n>=1 && ops[n-1].kind==PDFO_STR) {
                pdfshow(r, ops[n-1].s);
            } else if (op=="TJ" && n>=1 && ops[n-1].kind==PDFO_ARRAY) {
                PdfObj a = ops[n-1];
                for (i32 i = 0; i<a.len && !p->why.data; i++) {
                    if (a.items[i].kind == PDFO_STR) {
                        pdfshow(r, a.items[i].s);
                    } else {
                        pdfkern(r, pdfreal(a.items[i]));
                    }
                }
            } else if (op=="Do" && n>=1 && ops[n-1].kind==PDFO_NAME) {
                pdfdo(r, res, ops[n-1].s);
            } else if (op=="rg" && n>=3) {
                pdffill(r, ops+n-3, 3);
            } else if ((op=="sc" || op=="cs") && n>=1) {
                if (op == "cs") {
                    g->fill[0] = g->fill[1] = g->fill[2] = 0;
                } else {
                    pdffill(r, ops, n);
                }
            } else if (op == "BI") {
                pdfinline(r, &x, t.beg);
            }
            break;

        case 3:
            if (op == "scn") {
                pdffill(r, ops, n);
            } else if (op=="BDC" || op=="BMC") {
                r->mcdepth++;
                PdfObj props = n>=1 ? ops[n-1] : PdfObj{};
                if (op=="BDC" && props.kind==PDFO_NAME) {
                    props = pdfget(p, pdfget(p, res, "Properties"), props.s);
                }
                PdfObj at = pdfget(p, props, "ActualText");
                if (op=="BDC" && !r->actual && at.kind==PDFO_STR) {
                    // In abuf, as it lasts past this operator. The
                    // glyphs it covers are kept until its end, so that
                    // when it never ends, they are what is read.
                    r->actual    = r->mcdepth;
                    r->atext     = pdfstrtext(p, &p->op, at, PDF_NOTEMAX);
                    r->abuf      = r->abuf ? r->abuf : pdfnew<u8>(p, &p->page, PDF_NOTEMAX);
                    r->atext.len = r->abuf ? r->atext.len : 0;
                    copybytes(r->abuf, r->atext.data, r->atext.len);
                    r->atext.data = r->abuf;
                    r->ashown    = 0;
                    r->aend      = 0;
                    r->aunits    = r->units.len;
                    r->apool     = r->pool.len;
                    r->aunmapped = r->unmapped;
                }
            } else if (op == "EMC") {
                if (r->actual && r->mcdepth==r->actual) {
                    r->units.len = r->aunits;
                    r->pool.len  = r->apool;
                    r->unmapped  = r->aunmapped;
                    u32 cps[PDF_MAPMAX];
                    Str s = r->atext;
                    for (iz i = 0; i < s.len;) {
                        i32 k = 0;
                        for (; k<PDF_MAPMAX && i<s.len; k++) {
                            i32 c = utf8strict(s, &i);
                            cps[k] = c<0 ? 0xfffd : (u32)c;
                        }
                        f64 adv = i<s.len ? 0 : r->aend;
                        if (r->ashown) {
                            pdfunit(r, cps, k, r->ax, r->ay, r->adx, r->ady,
                                    adv, r->asize, 0);
                        } else {
                            PdfMat m = pdfmul(r->tm, g->ctm);
                            f64    dl = pdflen(m.a, m.b);
                            f64    sz = pdflen(m.c, m.d) * g->fs;
                            pdfunit(r, cps, k, m.e, m.f, dl>0 ? m.a/dl : 1,
                                    dl>0 ? m.b/dl : 0, 0, sz, 0);
                        }
                    }
                    r->actual = 0;
                }
                r->mcdepth -= r->mcdepth > 0;
            }
            break;
        }
        n = 0;
        pdfrewind(p, &mark);
        if (p->why.data) {
            break;
        }
    }
    pdfrewind(p, &mark);
}

// A page's contents, its streams decoded in page and run together with
// a space between, where each starts noted for inline images.
static Str pdfcontents(PdfRun *r, PdfObj contents)
{
    Pdf   *p    = r->p;
    PdfObj c    = pdfget(p, contents);
    PdfObj one  = contents;
    PdfObj *list = &one;
    i32    n    = 1;
    if (c.kind == PDFO_ARRAY) {
        list = c.items;
        n    = c.len;
    }
    if (!n) {
        return {};
    }
    r->starts = pdfnew<iz>(p, &p->page, n);
    r->nums   = pdfnew<i32>(p, &p->page, n);
    r->gens   = pdfnew<i32>(p, &p->page, n);
    Str *parts = pdfnew<Str>(p, &p->page, n);
    if (!r->starts || !r->nums || !r->gens || !parts) {
        return {};
    }
    iz total = 0;
    for (i32 i = 0; i<n && !p->why.data; i++) {
        PdfData d = pdfstream(p, &p->page, list[i], 0);
        parts[i] = d.image ? Str{} : d.data;
        r->cut  |= d.cut && !p->why.data;  // else the read's reason says so
        total   += parts[i].len + 1;
        r->nums[i] = list[i].kind==PDFO_REF ? list[i].ref.num : 0;
        r->gens[i] = list[i].kind==PDFO_REF ? list[i].ref.gen : 0;
    }
    r->nstreams = n;
    if (n == 1) {
        r->starts[0] = 0;
        return parts[0];
    }
    u8 *all = pdfnew<u8>(p, &p->page, total);
    if (!all) {
        return {};
    }
    iz at = 0;
    for (i32 i = 0; i < n; i++) {
        r->starts[i] = at;
        copybytes(all+at, parts[i].data, parts[i].len);
        at += parts[i].len;
        all[at++] = ' ';
    }
    return {all, at};
}


// Layout
//
// Glyph units become lines of text by simple heuristics, in this order:
//  1. Group by writing direction, the most common first. Rotated text is
//     its own group, read in its own frame; the page's /Rotate is not
//     looked at, as content is written in the page's own frame.
//  2. Lines by overlapping baselines, top to bottom: a unit joins if it
//     overlaps the line's largest unit by half the smaller's height, so
//     a superscript joins its line.
//  3. Within a line, order along it, a combining mark after its base
//     (the unit drawn before it).
//  4. Fake bold, a glyph drawn again a hair away, is dropped, each unit
//     compared with the 8 before it only.
//  5. A space where a gap is wider than a share of an em plus the line's
//     letter spacing: the lower quartile of its gaps between letters, so
//     that tracking does not read as spaces. Two at a wide gap.
//  6. Columns: bands of consecutive lines that share free intervals, the
//     gutters, with text on both sides in 3 lines or more. A gutter
//     beside a column whose cells average under 2.5 words is a table's,
//     and dropped, so that tables read in rows, cells two spaces apart.
//  7. Right-to-left runs, which lie left to right on the page, are put
//     in logical order, keeping the numbers in them in theirs.
//  8. A blank line where the baseline steps more than a paragraph's, and
//     between columns and groups; lines wrap under READ_LINEMAX.
// Layout charges nothing, as the glyphs it lays out were charged, and
// asks memory of page directly, so that a page cut short by a stopped
// read is still laid out.

struct PdfLine {
    iz  beg;   // units [beg, end), in order
    iz  end;
    f32 v;     // its baseline, in its group's frame
    f32 u0;    // its extent along it, spaces aside
    f32 u1;
    f32 size;  // its largest unit's
    i32 grp;
    b32 text;  // it has more than spaces
};

static b32 pdfunitless(PdfUnit *a, PdfUnit *b)
{
    if (a->grp != b->grp) return a->grp < b->grp;
    if (a->v != b->v)     return a->v > b->v;
    return a->seq < b->seq;
}

// Along a line, a combining mark after the unit at its place.
static b32 pdfunitalong(PdfUnit *a, PdfUnit *b)
{
    if (a->u != b->u) return a->u < b->u;
    i32 am = !!(a->flags & PDFU_MARK);
    i32 bm = !!(b->flags & PDFU_MARK);
    if (am != bm) return am < bm;
    return a->seq < b->seq;
}

static b32 pdfunitseq(PdfUnit *a, PdfUnit *b)
{
    return a->seq < b->seq;
}

template<typename T>
static T *pdflayoutnew(Arena *a, iz n)
{
    iz pad = -(uz)a->beg & (alignof(T) - 1);
    return pdffits(a, pad, n, sizeof(T)) ? alloc<T>(a, n) : 0;
}

// Sort without charging, if a has the room. Returns whether sorted.
template<typename T>
static b32 pdflayoutsort(Arena *a, T *v, iz n, b32 (*less)(T *, T *))
{
    iz pad = -(uz)a->beg & (alignof(T) - 1);
    if (n>1 && !pdffits(a, pad, n, sizeof(T))) {
        return 0;
    }
    sort(v, n, less, *a);
    return 1;
}

static f32 pdfmaxf(f32 a, f32 b)
{
    return a>b ? a : b;
}

// The combining mark a spacing accent stands for, as TeX draws one over
// a letter, or 0.
static u32 pdfaccent(Str s)
{
    static struct { u32 spacing, combining; } const accents[] = {
        {0x0060, 0x0300}, {0x00a8, 0x0308}, {0x00af, 0x0304}, {0x00b4, 0x0301},
        {0x00b8, 0x0327}, {0x02c6, 0x0302}, {0x02c7, 0x030c}, {0x02d8, 0x0306},
        {0x02d9, 0x0307}, {0x02da, 0x030a}, {0x02db, 0x0328}, {0x02dc, 0x0303},
        {0x02dd, 0x030b},
    };
    iz  i = 0;
    i32 c = utf8strict(s, &i);
    for (i32 k = 0; c>=0 && i==s.len && k<countof(accents); k++) {
        if ((u32)c == accents[k].spacing) {
            return accents[k].combining;
        }
    }
    return 0;
}

// A line, its units sorted along it: marks moved after their bases, fake
// bold dropped, spaces noted where gaps are (brk), and its extent. False
// if a sort had not the room, which leaves it unshaped: never shaped
// otherwise, as the text would then depend on the memory there was.
static b32 pdfshape(PdfRun *r, PdfLine *l)
{
    Arena   *a = &r->p->page;
    u8      *pool = r->pool.data;
    PdfUnit *v = r->units.data + l->beg;
    iz       n = l->end - l->beg;

    // A mark takes the place of the unit drawn before it
    b32 marks = 0;
    for (iz i = 0; i < n; i++) {
        marks |= v[i].flags & PDFU_MARK;
    }
    if (marks) {
        if (!pdflayoutsort(a, v, n, pdfunitseq)) {
            return 0;
        }
        for (iz i = 1; i < n; i++) {
            if (v[i].flags & PDFU_MARK) {
                v[i].u = v[i-1].u;
            }
        }
        if (!pdflayoutsort(a, v, n, pdfunitalong)) {
            return 0;
        }
    }

    // A spacing accent drawn over a letter beside it is the letter's
    // combining mark, over a dotless i or j the dotted one's. Its text
    // is rewritten in place, or in the pool's spare room for a grave.
    b32 accents = 0;
    for (iz i = 0; i < n; i++) {
        PdfUnit *x = v + i;
        u32      m = pdfaccent({pool+x->off, x->len});
        if (!m) {
            continue;
        }
        f32      mid  = x->u + x->adv/2;
        PdfUnit *base = 0;
        for (iz j = i-1; j<=i+1 && !base; j += 2) {
            PdfUnit *b = j>=0 && j<n ? v+j : 0;
            base = b && (b->flags&PDFU_LTR) && !(b->flags&(PDFU_MARK|PDFU_SPACE)) &&
                   !pdfaccent({pool+b->off, b->len}) && b->adv>0 &&
                   mid>=b->u && mid<=b->u+b->adv ? b : 0;
        }
        iz off = x->len==2 ? x->off : r->pool.cap-r->pool.len>=2 ? r->pool.len : -1;
        if (!base || off<0) {
            continue;
        }
        r->pool.len += off==r->pool.len ? 2 : 0;
        pool = r->pool.data;
        pool[off]   = (u8)(0xc0 | m>>6);
        pool[off+1] = (u8)(0x80 | (m & 0x3f));
        x->off   = (i32)off;
        x->len   = 2;
        x->flags = (u16)((x->flags & ~(PDFU_LTR|PDFU_RTL)) | PDFU_MARK);
        x->u     = base->u;
        x->adv   = 0;
        x->seq   = base->seq;  // after it, among its marks
        Str bt = {pool+base->off, base->len};
        if (bt=="\xc4\xb1" || bt=="\xc8\xb7") {  // dotless i, j
            pool[base->off] = bt[0]==0xc4 ? 'i' : 'j';
            base->len = 1;
        }
        accents = 1;
    }
    if (accents && !pdflayoutsort(a, v, n, pdfunitalong)) {
        return 0;
    }

    // Fake bold: the same text again, nearly in the same place
    iz k = 0;
    for (iz i = 0; i < n; i++) {
        PdfUnit *x   = v + i;
        b32      dup = 0;
        for (iz j = k-1; j>=0 && j>=k-PDF_BOLDWIN && !dup; j--) {
            PdfUnit *y  = v + j;
            f32      du = __builtin_fabsf(x->u - y->u);
            dup = du<PDF_BOLDEM*x->size && du<0.5f*y->adv &&
                  __builtin_fabsf(x->v - y->v)<PDF_BOLDEM*x->size &&
                  __builtin_fabsf(x->size - y->size)<=0.1f*x->size &&
                  Str{pool+x->off, x->len} == Str{pool+y->off, y->len};
        }
        if (!dup) {
            v[k++] = *x;
        }
    }
    l->end = l->beg + k;
    n      = k;

    // Letter spacing: the lower quartile of gaps between letters, in
    // hundredths of an em from -0.5
    i32 hist[150] = {};
    i32 pairs = 0;
    f32 end   = 0;
    for (iz i = 0; i < n; i++) {
        PdfUnit *x = v + i;
        if (i && !((x->flags|v[i-1].flags) & (PDFU_SPACE|PDFU_FIELD|PDFU_MARK))) {
            f32 gap = (x->u - end) / pdfmaxf(x->size, v[i-1].size);
            if (gap>-0.5f && gap<1.0f) {
                i32 b = (i32)((gap + 0.5f)*100);
                hist[b<149 ? b : 149]++;
                pairs++;
            }
        }
        f32 e = x->u + x->adv;
        end = !i || e>end ? e : end;
    }
    f32 track = 0;
    if (pairs >= 4) {
        i32 want = (i32)(PDF_TRACKQ*(f32)pairs);
        i32 b    = 0;
        for (i32 sum = 0; b<149 && (sum += hist[b])<=want; b++) {}
        track = ((f32)b + 0.5f)/100 - 0.5f;
        track = track<0 ? 0 : track>PDF_TRACKMAX ? PDF_TRACKMAX : track;
    }

    // Breaks, and the extent
    f32 u0 = 0, u1 = 0, size = 0;
    b32 text = 0;
    end = 0;
    for (iz i = 0; i < n; i++) {
        PdfUnit *x = v + i;
        x->brk = 0;
        if (i) {
            PdfUnit *w = v + i - 1;
            u16      f = x->flags | w->flags;
            if (!(f & PDFU_SPACE) && !(x->flags & PDFU_MARK)) {
                f32 gap = (x->u - end) / pdfmaxf(x->size, w->size);
                f32 th  = (x->flags & w->flags & PDFU_CJK) ? PDF_CJKEM : PDF_SPACEEM;
                x->brk  = gap>PDF_WIDEEM+track ? 2 : gap>th+track ? 1 : 0;
                x->brk  = (f & PDFU_FIELD) && !x->brk ? 1 : x->brk;
            }
        }
        f32 e = x->u + x->adv;
        end = !i || e>end ? e : end;
        if (!(x->flags & PDFU_SPACE)) {
            f32 lo = x->u<e ? x->u : e;
            f32 hi = x->u<e ? e : x->u;
            u0   = !text || lo<u0 ? lo : u0;
            u1   = !text || hi>u1 ? hi : u1;
            text = 1;
        }
        size = pdfmaxf(size, x->size);
    }
    l->u0   = u0;
    l->u1   = u1;
    l->size = size;
    l->text = text;
    return 1;
}

// Lines of the page's units, which are reordered, in page. Null if the
// page has not the room.
static Slice<PdfLine> pdflines(PdfRun *r)
{
    Arena   *a = &r->p->page;
    PdfUnit *u = r->units.data;
    iz       n = r->units.len;
    Slice<PdfLine> lines = {};

    // Directions, quantized: the most common first, then by appearance
    i32 keys[PDF_GROUPS] = {};
    i32 count[PDF_GROUPS] = {};
    f32 gdx[PDF_GROUPS] = {};
    f32 gdy[PDF_GROUPS] = {};
    i32 ng = 0;
    for (iz i = 0; i < n; i++) {
        i32 qx  = pdftoi32(u[i].dx*16 + (u[i].dx<0 ? -0.5f : 0.5f));
        i32 qy  = pdftoi32(u[i].dy*16 + (u[i].dy<0 ? -0.5f : 0.5f));
        i32 key = qx*64 + qy;
        i32 g   = 0;
        for (; g<ng && keys[g]!=key; g++) {}
        if (g == ng) {
            if (ng < PDF_GROUPS) {
                keys[ng] = key;
                gdx[ng]  = u[i].dx;
                gdy[ng]  = u[i].dy;
                ng++;
            } else {
                g = ng - 1;
            }
        }
        count[g]++;
        u[i].grp = g;
    }
    i32 rank[PDF_GROUPS] = {};
    for (i32 g = 0; g < ng; g++) {
        for (i32 h = 0; h < ng; h++) {
            rank[g] += count[h]>count[g] || (count[h]==count[g] && h<g);
        }
    }
    for (iz i = 0; i < n; i++) {
        i32 g = u[i].grp;
        u[i].u   = u[i].x*gdx[g] + u[i].y*gdy[g];
        u[i].v   = u[i].y*gdx[g] - u[i].x*gdy[g];
        u[i].grp = rank[g];
    }
    if (!pdflayoutsort(a, u, n, pdfunitless)) {
        return {};
    }

    // Lines: a unit joins if it overlaps the line's largest unit by half
    // the height of the smaller
    PdfLine *ls = pdflayoutnew<PdfLine>(a, n);
    iz       nl = 0;
    if (!ls && n) {
        return {};
    }
    f32 clo = 0, chi = 0, cs = 0;
    for (iz i = 0; i < n; i++) {
        f32 s  = u[i].size;
        f32 lo = u[i].v - 0.25f*s;
        f32 hi = u[i].v + 0.75f*s;
        f32 ov = (hi<chi ? hi : chi) - (lo>clo ? lo : clo);
        if (nl && ls[nl-1].grp==u[i].grp && ov>=PDF_OVERLAP*(s<cs ? s : cs)) {
            ls[nl-1].end = i + 1;
            if (s > cs) {
                clo = lo, chi = hi, cs = s;
                ls[nl-1].v = u[i].v;
            }
            continue;
        }
        ls[nl]     = {};
        ls[nl].beg = i;
        ls[nl].end = i + 1;
        ls[nl].v   = u[i].v;
        ls[nl].grp = u[i].grp;
        nl++;
        clo = lo, chi = hi, cs = s;
    }
    for (iz k = 0; k < nl; k++) {
        if (!pdflayoutsort(a, u+ls[k].beg, ls[k].end-ls[k].beg, pdfunitalong)) {
            return {};
        }
    }

    // A taller unit can join two baselines of another column into one
    // line, whose letters then lie between each other. Such a line, its
    // letters overlapping their neighbours' on other baselines, is cut
    // into one line per baseline.
    PdfLine *out = 0;
    iz       no  = 0;
    for (iz k = 0; k < nl; k++) {
        PdfLine l = ls[k];
        iz      m = 0;  // letters
        iz      c = 0;  // ...overlapping the one before on another baseline
        for (iz i = l.beg+1; i < l.end; i++) {
            PdfUnit *x = u + i - 1;
            PdfUnit *y = u + i;
            if ((x->flags|y->flags) & (PDFU_SPACE|PDFU_MARK)) {
                continue;
            }
            f32 sm = x->size<y->size ? x->size : y->size;
            m++;
            c += x->u+x->adv-y->u > PDF_TANGLE*sm &&
                 __builtin_fabsf(x->v - y->v) > PDF_BASELINE*sm;
        }
        if (c<4 || 4*c<m) {
            if (out) {
                out[no++] = l;
            }
            continue;
        }
        if (!out) {
            out = pdflayoutnew<PdfLine>(a, n);
            if (!out) {
                return {};  // no room: lost, not left tangled
            }
            for (iz j = 0; j < k; j++) {
                out[no++] = ls[j];
            }
        }
        if (!pdflayoutsort(a, u+l.beg, l.end-l.beg, pdfunitless)) {
            return {};
        }
        for (iz i = l.beg; i < l.end;) {
            iz  b   = i++;
            f32 big = u[b].size;
            f32 v   = u[b].v;
            for (; i<l.end; i++) {
                f32 sm = u[i-1].size<u[i].size ? u[i-1].size : u[i].size;
                if (u[i-1].v-u[i].v > PDF_BASELINE*sm) {
                    break;
                }
                v   = u[i].size>big ? u[i].v : v;
                big = u[i].size>big ? u[i].size : big;
            }
            if (!pdflayoutsort(a, u+b, i-b, pdfunitalong)) {
                return {};
            }
            out[no]     = {};
            out[no].beg = b;
            out[no].end = i;
            out[no].v   = v;
            out[no].grp = l.grp;
            no++;
        }
    }
    if (out) {
        ls = out;
        nl = no;
    }
    for (iz k = 0; k < nl; k++) {
        if (!pdfshape(r, ls+k)) {
            return {};
        }
    }
    lines.data = ls;
    lines.len  = lines.cap = nl;
    return lines;
}

// Columns

struct PdfGap {
    f32 lo, hi;
};

// Lines that share gutters: the columns between them (none if it reads
// in rows), and the cells' gutters, columns' too, where a line crossing
// one has two spaces.
struct PdfBand {
    iz      beg;   // lines
    iz      end;
    PdfGap *col;
    PdfGap *cell;
    i32     ncol;
    i32     ncell;
    b32     rtl;   // columns right to left
};

static f32 pdfmid(PdfGap g)
{
    return g.lo/2 + g.hi/2;
}

static f32 pdfcentre(PdfUnit *x)
{
    return x->u + x->adv/2;
}

// How wide a gutter is beside line l, at least: of its size, but no
// more than of the body's, so that a heading's line does not need one
// wider than the body's columns have.
static f32 pdfgutterw(PdfLine *l, f32 body)
{
    return PDF_GUTTEREM * (l->size<body ? l->size : body);
}

// The intervals of [lo, hi] a line leaves free, at least a gutter wide,
// at most PDF_GAPS, in order.
static i32 pdffree(PdfUnit *u, PdfLine *l, f32 lo, f32 hi, PdfGap *out, f32 body)
{
    f32 w  = pdfgutterw(l, body);
    f32 at = lo;
    i32 n  = 0;
    for (iz i = l->beg; i < l->end; i++) {
        if (u[i].flags & PDFU_SPACE) {
            continue;
        }
        f32 a = u[i].u;
        f32 b = u[i].u + u[i].adv;
        if (b < a) {
            f32 c = a;
            a = b;
            b = c;
        }
        if (a-at>=w && n<PDF_GAPS) {
            out[n++] = {at, a};
        }
        at = b>at ? b : at;
    }
    if (hi-at>=w && n<PDF_GAPS) {
        out[n++] = {at, hi};
    }
    return n;
}

// Where two lists of intervals meet, at least w wide.
static i32 pdfmeet(PdfGap *a, i32 na, PdfGap *b, i32 nb, f32 w, PdfGap *out)
{
    i32 n = 0;
    for (i32 i = 0, j = 0; i<na && j<nb;) {
        f32 lo = a[i].lo>b[j].lo ? a[i].lo : b[j].lo;
        f32 hi = a[i].hi<b[j].hi ? a[i].hi : b[j].hi;
        if (hi-lo>=w && n<PDF_GAPS) {
            out[n++] = {lo, hi};
        }
        if (a[i].hi < b[j].hi) {
            i++;
        } else {
            j++;
        }
    }
    return n;
}

// The gutters among in, inside [lo, hi], that lines [beg, end) have text
// on both sides of in PDF_GUTLINES or more, into out (may be in).
static i32 pdfgutters(PdfLine *L, iz beg, iz end, PdfGap *in, i32 nin,
                      f32 lo, f32 hi, PdfGap *out)
{
    i32 n = 0;
    for (i32 j = 0; j < nin; j++) {
        PdfGap g = in[j];
        if (!(g.lo>lo && g.hi<hi)) {
            continue;
        }
        i32 left = 0, right = 0;
        for (iz k = beg; k < end; k++) {
            left  += L[k].text && L[k].u0<g.lo;
            right += L[k].text && L[k].u1>g.hi;
        }
        if (left>=PDF_GUTLINES && right>=PDF_GUTLINES && n<PDF_COLUMNS) {
            out[n++] = g;
        }
    }
    return n;
}

// The cells and words of each column that gutters g make of lines
// [beg, end): a cell is a line's text in a column.
static void pdfwords(PdfUnit *u, PdfLine *L, iz beg, iz end, PdfGap *g, i32 ng,
                     i32 *cells, i32 *words)
{
    for (i32 c = 0; c <= ng; c++) {
        cells[c] = words[c] = 0;
    }
    for (iz k = beg; k < end; k++) {
        i32 c    = 0;
        i32 last = -1;
        b32 sp   = 0;
        for (iz i = L[k].beg; i < L[k].end; i++) {
            PdfUnit *x = u + i;
            if (x->flags & PDFU_SPACE) {
                sp = 1;
                continue;
            }
            for (; c<ng && pdfcentre(x)>pdfmid(g[c]); c++) {}
            if (c != last) {
                cells[c]++;
                words[c]++;
                last = c;
            } else if (sp || x->brk) {
                words[c]++;
            }
            sp = 0;
        }
    }
}

// A band tried from a line: where it ends, its columns' gutters, and
// the gutters it had before the table test dropped some, its cells'.
struct PdfBandTry {
    iz     end;
    i32    ncol;
    i32    ncell;
    PdfGap col[PDF_COLUMNS];
    PdfGap cell[PDF_COLUMNS];
};

// The band from line s of [s, ge), in the group's extent [lo, hi]: the
// lines that share a free interval inside it, taken greedily. With
// gutters, lines after the last with text right of the first are only
// the first column's, and stay in the band until a step a paragraph's
// size, after which they read after every column (a heading below).
// Visits to lines are counted, so that a page of many lines, cut and
// taken again, stays linear.
static PdfBandTry pdfband(PdfUnit *u, PdfLine *L, iz s, iz ge, f32 lo, f32 hi,
                          f32 body, i64 *visits)
{
    PdfBandTry t = {};
    PdfGap     c[PDF_GAPS], f[PDF_GAPS], x[PDF_GAPS];
    i32        nc = pdffree(u, L+s, lo, hi, c, body);
    iz         i  = s + 1;
    for (; i<ge && *visits>0; i++, (*visits)--) {
        i32 nf    = pdffree(u, L+i, lo, hi, f, body);
        i32 nx    = pdfmeet(c, nc, f, nf, pdfgutterw(L+i, body), x);
        b32 inner = 0;
        for (i32 j = 0; j < nx; j++) {
            inner |= x[j].lo>lo && x[j].hi<hi;
        }
        if (!inner) {
            break;
        }
        for (i32 j = 0; j < nx; j++) {
            c[j] = x[j];
        }
        nc = nx;
    }

    // Tables: drop a gutter beside a column of short cells, first where
    // both sides are short. The cells' gutters are those before.
    PdfGap *g      = t.col;
    i32     ng     = pdfgutters(L, s, i, c, nc, lo, hi, g);
    auto    tables = [&] {
        t.ncell = ng;
        for (i32 j = 0; j < ng; j++) {
            t.cell[j] = g[j];
        }
        for (;;) {
            i32 cells[PDF_COLUMNS+1], words[PDF_COLUMNS+1];
            b32 shorts[PDF_COLUMNS+1];
            pdfwords(u, L, s, i, g, ng, cells, words);
            for (i32 j = 0; j <= ng; j++) {
                shorts[j] = (f32)words[j] < PDF_PROSE*(f32)cells[j];
            }
            i32 drop = -1;
            for (i32 j = 0; j<ng && drop<0; j++) {
                drop = shorts[j] && shorts[j+1] ? j : -1;
            }
            for (i32 j = 0; j<ng && drop<0; j++) {
                drop = shorts[j] || shorts[j+1] ? j : -1;
            }
            if (drop < 0) {
                break;
            }
            for (i32 j = drop; j < ng-1; j++) {
                g[j] = g[j+1];
            }
            ng--;
        }
    };
    tables();

    // Past the last line with text right of the first column (of those
    // the table test left), the first column's lines
    if (ng) {
        iz last = s;
        for (iz k = s; k < i; k++) {
            last = L[k].text && L[k].u1>g[0].hi ? k : last;
        }
        for (last++; last < i; last++) {
            f32 em = pdfmaxf(L[last-1].size, L[last].size);
            if (L[last-1].v-L[last].v > PDF_PARAEM*em) {
                break;  // a step: no longer the first column's
            }
        }
        if (last < i) {
            // Gutters lines cut off had narrowed come back
            i  = last;
            nc = pdffree(u, L+s, lo, hi, c, body);
            for (iz k = s+1; k < i; k++) {
                i32 nf = pdffree(u, L+k, lo, hi, f, body);
                nc = pdfmeet(c, nc, f, nf, pdfgutterw(L+k, body), x);
                for (i32 j = 0; j < nc; j++) {
                    c[j] = x[j];
                }
            }
            *visits -= i - s;
            ng = pdfgutters(L, s, i, c, nc, lo, hi, g);
            tables();
        }
    }
    t.end  = i;
    t.ncol = ng;
    return t;
}

// The bands of a group's lines [gb, ge), into out (room for one per
// line), and how many. When the band from the line after finds more
// gutters, a line is a band of its own: a heading over columns that
// reaches into a gutter does not narrow it away. Or -1 if a band's
// gutters had not the room, rather than bands in rows that need none.
static iz pdfbands(PdfRun *r, PdfLine *L, iz gb, iz ge, PdfBand *out)
{
    Arena   *a  = &r->p->page;
    PdfUnit *u  = r->units.data;
    f32      lo = 0, hi = 0;
    b32      any = 0;
    for (iz k = gb; k < ge; k++) {
        if (L[k].text) {
            lo  = !any || L[k].u0<lo ? L[k].u0 : lo;
            hi  = !any || L[k].u1>hi ? L[k].u1 : hi;
            any = 1;
        }
    }

    // The body's size: that of the most lines, by half points, among
    // the first PDF_SIZES seen
    f32 sizes[PDF_SIZES];
    iz  count[PDF_SIZES];
    i32 nsizes = 0;
    for (iz k = gb; k < ge; k++) {
        if (!L[k].text) {
            continue;
        }
        f32 q = (f32)pdftoi32(2*L[k].size + 0.5f) / 2;
        i32 j = 0;
        for (; j<nsizes && sizes[j]!=q; j++) {}
        if (j==nsizes && nsizes<PDF_SIZES) {
            sizes[nsizes] = q;
            count[nsizes++] = 0;
        }
        count[j] += j < nsizes;
    }
    f32 body = 3.4e38f;
    for (i32 j = 0, most = 0; j < nsizes; j++) {
        body = count[j]>most ? sizes[j] : body;
        most = count[j]>most ? (i32)count[j] : most;
    }

    iz         nb     = 0;
    i64        visits = 8*(i64)(ge - gb) + 64;
    PdfBandTry cur    = {};
    PdfBandTry next   = {};
    b32        have   = 0;  // next is the band from s
    for (iz s = gb; s < ge;) {
        cur  = have ? next : pdfband(u, L, s, ge, lo, hi, body, &visits);
        have = 0;
        if (cur.end > s+1) {
            next = pdfband(u, L, s+1, ge, lo, hi, body, &visits);
            if (next.ncell > cur.ncell) {
                cur     = {};
                cur.end = s + 1;
                have    = 1;
            }
        }

        PdfBand *b = out + nb++;
        *b = {};
        b->beg  = s;
        b->end  = cur.end;
        b->col  = cur.ncol ? pdflayoutnew<PdfGap>(a, cur.ncol) : 0;
        b->cell = cur.ncell ? pdflayoutnew<PdfGap>(a, cur.ncell) : 0;
        if ((cur.ncol && !b->col) || (cur.ncell && !b->cell)) {
            return -1;
        }
        b->ncol = cur.ncol;
        for (i32 j = 0; j < b->ncol; j++) {
            b->col[j] = cur.col[j];
        }
        b->ncell = cur.ncell;
        for (i32 j = 0; j < b->ncell; j++) {
            b->cell[j] = cur.cell[j];
        }
        i32 rtl = 0, all = 0;
        for (iz k = s; b->ncol && k < b->end; k++) {
            for (iz j = L[k].beg; j < L[k].end; j++) {
                rtl += !!(u[j].flags & PDFU_RTL);
                all += !!(u[j].flags & (PDFU_RTL|PDFU_LTR));
            }
        }
        b->rtl = 2*rtl > all;

        s = b->end;
    }
    return nb;
}

// The units of line l in column c of gutters g, [*a, *e), whether any is
// text, and their extent.
static b32 pdfcolumn(PdfUnit *u, PdfLine *l, PdfGap *g, i32 ng, i32 c,
                     iz *a, iz *e, f32 *u0, f32 *u1)
{
    f32 lo = c    ? pdfmid(g[c-1]) : -3.4e38f;
    f32 hi = c<ng ? pdfmid(g[c])   :  3.4e38f;
    iz  i  = l->beg;
    for (; i<l->end && pdfcentre(u+i)<=lo; i++) {}
    *a = i;
    b32 text = 0;
    for (; i<l->end && pdfcentre(u+i)<=hi; i++) {
        if (!(u[i].flags & PDFU_SPACE)) {
            f32 x0 = u[i].u;
            f32 x1 = u[i].u + u[i].adv;
            *u0  = !text || x0<*u0 ? x0 : *u0;
            *u1  = !text || x1>*u1 ? x1 : *u1;
            text = 1;
        }
    }
    *e = i;
    return text;
}


// Page output

// The text being made, in perm: notes always fit under PDF_TEXTMAX, as
// the extracted text stops PDF_NOTEROOM short of it. Lines are written
// a piece at a time.
struct PdfText {
    Buf b;
    iz  lines;      // complete lines so far
    iz  ls;         // where the line being written starts
    b32 blank;      // the last line is blank, or a page's marker
    b32 wantblank;  // a blank line goes before the next line
    Str why;        // why the text stopped, if it did
};

// Room for need more bytes, without regard to a stopped read: or stop
// the text, keeping notes' room unless note.
static b32 pdftextroom(PdfText *t, iz need, b32 note)
{
    Buf *b   = &t->b;
    iz   max = PDF_TEXTMAX - (note ? 0 : PDF_NOTEROOM);
    if (t->why.data && !note) {
        return 0;
    } else if (need > max-b->len) {
        t->why = t->why.data ? t->why : Str("the text is longer than is allowed");
        return 0;
    } else if (b->cap-b->len >= need) {
        return 1;
    }
    Arena *a      = b->a;
    iz     extend = b->cap>need ? b->cap : need;
    b32    front  = (byte *)(b->data+b->cap) == a->beg;
    iz     room   = pdfroom(a);
    if (extend>=room || (!front && b->cap>=room-extend)) {
        t->why = t->why.data ? t->why : Str("out of memory");
        return 0;
    }
    reserve(b, need);
    return 1;
}

// Whether a line would read as a page marker.
static b32 pdfmarkerlike(Str s)
{
    return startswith(s, "--- page ");
}

// End the line being written, if any: trailing spaces trimmed, and a
// leading space if it would read as a page marker.
static void pdfwend(PdfText *t)
{
    Buf *b = &t->b;
    while (b->len>t->ls && b->data[b->len-1]==' ') {
        b->len--;
    }
    if (b->len == t->ls) {
        return;
    }
    iz len = b->len - t->ls;
    if (pdfmarkerlike({b->data+t->ls, len})) {
        if (!pdftextroom(t, 1, 0)) {
            b->len = t->ls;
            return;
        }
        __builtin_memmove(b->data+t->ls+1, b->data+t->ls, (uz)len);
        b->data[t->ls] = ' ';
        b->len++;
    }
    if (!pdftextroom(t, 1, 0)) {
        b->len = t->ls;
        return;
    }
    putbyte(b, '\n');
    t->lines++;
    t->ls    = b->len;
    t->blank = 0;
}

// Write clean text s to the line, after sp spaces (0, 1 or 2) unless it
// starts the line: a blank line first if one is wanted, and the line
// wrapped before s at a break past PDF_LINEWRAP, or anywhere past
// PDF_LINEHARD. Pieces longer than PDF_PIECEMAX are split. Returns
// whether it was written.
static b32 pdfwput(PdfText *t, Str s, i32 sp)
{
    Buf *b = &t->b;
    while (s.len > PDF_PIECEMAX) {
        iz n = PDF_PIECEMAX;
        for (; n>1 && (s[n]&0xc0)==0x80; n--) {}
        if (!pdfwput(t, takehead(s, n), sp)) {
            return 0;
        }
        s  = cuthead(s, n);
        sp = 0;
    }
    if (b->len == t->ls) {
        sp = 0;
        if (t->wantblank && !t->blank) {
            if (!pdftextroom(t, 1, 0)) {
                return 0;
            }
            putbyte(b, '\n');
            t->lines++;
            t->ls    = b->len;
            t->blank = 1;
        }
        t->wantblank = 0;
    }
    iz cur = b->len - t->ls;
    if ((sp && cur+sp+s.len>PDF_LINEWRAP) || cur+s.len>PDF_LINEHARD) {
        pdfwend(t);
        sp = 0;
    }
    if (!pdftextroom(t, sp+s.len, 0)) {
        return 0;
    }
    for (i32 i = 0; i < sp; i++) {
        putbyte(b, ' ');
    }
    print(b, s);
    return 1;
}

// Write clean text s after sp spaces, its runs of spaces as breaks, long
// words in pieces.
static void pdfwwords(PdfText *t, Str s, i32 sp)
{
    for (iz i = 0; i < s.len;) {
        if (s[i] == ' ') {
            sp = 1;
            i++;
            continue;
        }
        iz j = i;
        for (; j<s.len && s[j]!=' ' && j-i<PDF_PIECEMAX; j++) {}
        for (; j<s.len && j>i+1 && (s[j]&0xc0)==0x80; j--) {}
        if (!pdfwput(t, {s.data+i, j-i}, sp)) {
            return;
        }
        sp = 0;
        i  = j;
    }
}

// A whole line of notes, after the line being written.
static void pdfnote(PdfText *t, Str s)
{
    pdfwend(t);
    if (pdftextroom(t, s.len+1, 1)) {
        print(&t->b, s);
        putbyte(&t->b, '\n');
        t->lines++;
        t->ls    = t->b.len;
        t->blank = 0;
    }
    t->wantblank = 0;
}

// Scratch for writing a line's units: their order and breaks.
struct PdfSeg {
    i32 *ord;
    i32 *ord2;
    i32 *atom;
    u8  *brk;
    u8  *brk2;
};

enum : u8 { PDF_INRTL = 4 };  // with a break: the unit is in a right-to-left run

// A bracket mirrored, as it reads in a right-to-left run.
static Str pdfmirror(Str s)
{
    static char const pairs[] = "()[]{}<>";
    for (i32 i = 0; i < 8; i++) {
        if (s.len==1 && s[0]==pairs[i]) {
            return {(u8 *)pairs + (i^1), 1};
        }
    }
    if (s == Str("«")) return "»";
    if (s == Str("»")) return "«";
    return s;
}

// Put the units of a right-to-left run, ord[f..l], in logical order:
// reversed, but a number's digits kept in order and marks after their
// bases, and in a right-to-left line, left-to-right runs in order too:
// from a letter of one through its last letter, digit or mark before
// the next right-to-left letter. Each break goes with the gap it was.
static void pdfreverse(PdfUnit *u, PdfSeg *sg, iz f, iz l)
{
    i32 *ord = sg->ord;
    u8  *brk = sg->brk;
    iz   na  = 0;
    for (iz k = f; k <= l;) {
        iz s = k++;
        if (u[ord[s]].flags & PDFU_LTR) {
            iz end = k;
            for (iz j = k; j<=l && !(u[ord[j]].flags & PDFU_RTL); j++) {
                u16 g = u[ord[j]].flags;
                end = g&(PDFU_LTR|PDFU_DIGIT) || (g&PDFU_MARK && j==end) ? j+1 : end;
            }
            k = end;
        } else if (u[ord[s]].flags & PDFU_DIGIT) {
            for (; k<=l && !(brk[k]&3); k++) {
                u16 g = u[ord[k]].flags;
                if (g & PDFU_DIGIT) {
                    continue;
                } else if ((g & PDFU_NUMSEP) && k+1<=l && !(brk[k+1]&3) &&
                           (u[ord[k+1]].flags & PDFU_DIGIT)) {
                    continue;
                }
                break;
            }
        } else {
            for (; k<=l && !(brk[k]&3) && (u[ord[k]].flags & PDFU_MARK); k++) {}
        }
        sg->atom[na++] = (i32)s;
    }
    iz o = 0;
    for (iz q = na-1; q >= 0; q--) {
        iz s = sg->atom[q];
        iz e = q+1<na ? sg->atom[q+1] : l+1;
        u8 in = u[ord[s]].flags&PDFU_LTR ? 0 : PDF_INRTL;
        for (iz k = s; k < e; k++, o++) {
            sg->ord2[o] = ord[k];
            sg->brk2[o] = (u8)((k>s ? brk[k] : q==na-1 ? brk[f] : brk[e]) | in);
        }
    }
    for (iz k = 0; k < o; k++) {
        ord[f+k] = sg->ord2[k];
        brk[f+k] = sg->brk2[k];
    }
}

// Write units [a, e) of a line, in order along it, as a line of text: a
// right-to-left run in logical order, its breaks as spaces, two where it
// crosses a cell's gutter, unmapped glyphs in a row as one U+FFFD.
static void pdfputseg(PdfText *t, PdfRun *r, PdfSeg *sg, iz a, iz e, PdfGap *cell, i32 ncell)
{
    PdfUnit *u   = r->units.data;
    i32     *ord = sg->ord;
    u8      *brk = sg->brk;
    iz       n   = e - a;
    for (iz k = 0; k < n; k++) {
        ord[k] = (i32)(a + k);
        brk[k] = u[a+k].brk;
    }
    i32 c = 0;
    for (; n && c<ncell && pdfmid(cell[c])<pdfcentre(u+a); c++) {}
    for (iz k = 1; k < n; k++) {
        b32 cross = 0;
        for (; c<ncell && pdfmid(cell[c])<pdfcentre(u+ord[k]); c++) {
            cross = 1;
        }
        brk[k] = cross ? 2 : brk[k];
    }

    // A right-to-left line, its letters at both ends right-to-left, is
    // reversed, but its runs of left-to-right ones. Else each run of
    // right-to-left letters, from the first to the last before a
    // left-to-right one, is reversed alone.
    b32 nrtl  = 0;
    u16 head  = 0;  // the first letter's direction, and the last's
    u16 last  = 0;
    for (iz k = 0; k < n; k++) {
        u16 fl = u[ord[k]].flags & (PDFU_RTL|PDFU_LTR);
        nrtl |= fl & PDFU_RTL;
        head  = head ? head : fl;
        last  = fl ? fl : last;
    }
    if (nrtl && head==PDFU_RTL && last==PDFU_RTL) {
        pdfreverse(u, sg, 0, n-1);
    } else if (nrtl) {
        for (iz k = 0; k < n;) {
            if (!(u[ord[k]].flags & PDFU_RTL)) {
                k++;
                continue;
            }
            iz f = k;
            iz l = k;
            for (k++; k<n && !(u[ord[k]].flags & PDFU_LTR); k++) {
                u16 g = u[ord[k]].flags;
                l = g&PDFU_RTL || (g&PDFU_MARK && l==k-1) ? k : l;
            }
            pdfreverse(u, sg, f, l);
            k = l + 1;
        }
    }

    i32 pend  = 0;
    b32 bad   = 0;
    b32 first = 1;
    for (iz k = 0; k < n; k++) {
        PdfUnit *x = u + ord[k];
        i32      b = brk[k] & 3;
        if (x->flags & PDFU_SPACE) {
            pend = b>pend ? b : pend>0 ? pend : 1;
            bad  = 0;
            continue;
        }
        i32 sp = first ? 0 : b>pend ? b : pend;
        if (x->flags & PDFU_UNMAPPED) {
            if (bad && !sp) continue;  // a run is one U+FFFD
            bad = 1;
        } else {
            bad = 0;
        }
        Str s = {r->pool.data+x->off, x->len};
        s = brk[k]&PDF_INRTL ? pdfmirror(s) : s;
        if (!pdfwput(t, s, sp)) {
            return;
        }
        first = 0;
        pend  = 0;
    }
    pdfwend(t);
}

// An image's placeholder line, unless the page has one for its number.
static void pdfputplace(PdfText *t, PdfRun *r, iz k)
{
    PdfPlace *pl = r->places.data + k;
    for (iz j = 0; j < r->places.len; j++) {
        if (j!=k && r->places.data[j].id==pl->id && r->places.data[j].line>=0) {
            pl->line = r->places.data[j].line;
            return;
        }
    }
    u8  tmp[160];
    Buf b = {};
    b.data = tmp;
    b.cap  = countof(tmp);
    b.a    = &r->p->op;  // never grows: the label is at most 34 bytes
    print(&b, "[image ");
    print(&b, (i64)pl->id);
    print(&b, ": ");
    print(&b, pl->image==PDFI_JPX || pl->image==PDFI_JBIG2
              ? cut(pl->label, ' ').head : pl->label);
    if (pl->wholepage) {
        print(&b, ", the whole page");
    }
    if (pl->image == PDFI_JPX) {
        print(&b, ", not shown (JPEG 2000)");
    } else if (pl->image == PDFI_JBIG2) {
        print(&b, ", not shown (JBIG2)");
    }
    putbyte(&b, ']');
    pdfwend(t);
    if (pdfwput(t, finish(&b), 0)) {
        iz at = t->lines;  // after a blank line put before it
        pdfwend(t);
        pl->line = t->lines==at+1 ? t->lines : pl->line;
    }
}

// The page's images, top down, put before lines as they are written.
struct PdfPlacing {
    iz  *order;
    u8  *done;
    iz  *nx;    // the next not done after each, or -1
    iz   np;
    iz   head;  // the first not done, or -1
    b32  on;    // in the main group, horizontal: images go among its lines
};

// Images before a line at v over [u0, u1]: those whose tops are above
// it and that overlap it, passing over at most PDF_PLACESCAN others.
static void pdfplacebefore(PdfText *t, PdfRun *r, PdfPlacing *pp, f32 v, f32 u0, f32 u1)
{
    i32 passed = 0;
    iz *link   = &pp->head;
    for (iz j = *link; pp->on && j>=0 && passed<PDF_PLACESCAN; j = *link) {
        PdfPlace *pl = r->places.data + pp->order[j];
        if (pl->y1 <= v) {
            break;
        } else if (pl->x0<=u1 && pl->x1>=u0) {
            pdfputplace(t, r, pp->order[j]);
            pp->done[j] = 1;
            *link = pp->nx[j];
        } else {
            passed++;
            link = pp->nx + j;
        }
    }
}

// Steps between a flow's lines, in ems, for its paragraphs.
struct PdfSteps {
    i32 hist[81];  // twentieths of an em, the last for 4 or more
    i32 n;
    f32 v;
    f32 size;
    b32 any;
};

static void pdfstep(PdfSteps *s, f32 v, f32 size)
{
    if (s->any) {
        f32 q = (s->v - v) / pdfmaxf(size, s->size);
        s->hist[q<0 ? 0 : q>=4 ? 80 : (i32)(q*20)]++;
        s->n++;
    }
    s->v    = v;
    s->size = size;
    s->any  = 1;
}

// The step, in ems, past which a flow's line starts a paragraph.
static f32 pdfparathr(PdfSteps *s)
{
    if (s->n < 2) {
        return PDF_PARAEM;
    }
    i32 b = 0;
    for (i32 sum = 0; b<80 && 2*(sum += s->hist[b])<=s->n; b++) {}
    f32 thr = PDF_PARASTEP * ((f32)b + 0.5f)/20;
    return thr<PDF_PARAMIN ? PDF_PARAMIN : thr;
}

// Before a line at v of size: a blank line if it steps past thr.
static void pdfputflow(PdfText *t, PdfSteps *fl, f32 thr, f32 v, f32 size)
{
    if (fl->any && (fl->v - v)/pdfmaxf(size, fl->size) > thr) {
        t->wantblank = 1;
    }
    fl->v    = v;
    fl->size = size;
    fl->any  = 1;
}

// A group's lines [gb, ge), by bands: lines in rows, or columns in turn.
// False, putting nothing, if its bands had not the room.
static b32 pdfputgroup(PdfText *t, PdfRun *r, PdfLine *L, iz gb, iz ge,
                       PdfSeg *sg, PdfPlacing *pp, PdfBand *bands)
{
    PdfUnit *u  = r->units.data;
    iz       nb = pdfbands(r, L, gb, ge, bands);
    if (nb < 0) {
        return 0;
    }
    PdfSteps fl = {};
    for (iz bi = 0; bi < nb;) {
        PdfBand *b = bands + bi;
        if (!b->ncol) {
            // A run of bands in rows, one flow
            iz       bj = bi;
            PdfSteps st = {};
            for (; bj<nb && !bands[bj].ncol; bj++) {
                for (iz k = bands[bj].beg; k < bands[bj].end; k++) {
                    if (L[k].text) pdfstep(&st, L[k].v, L[k].size);
                }
            }
            f32 thr = pdfparathr(&st);
            for (; bi < bj; bi++) {
                PdfBand *c = bands + bi;
                for (iz k = c->beg; k < c->end; k++) {
                    PdfLine *l = L + k;
                    if (l->text) {
                        pdfputflow(t, &fl, thr, l->v, l->size);
                        pdfplacebefore(t, r, pp, l->v, l->u0, l->u1);
                        pdfputseg(t, r, sg, l->beg, l->end, c->cell, c->ncell);
                    }
                }
            }
            continue;
        }

        // Columns in turn, each a flow, a blank line before each
        for (i32 j = 0; j <= b->ncol; j++) {
            i32      c  = b->rtl ? b->ncol-j : j;
            PdfSteps st = {};
            for (iz k = b->beg; k < b->end; k++) {
                iz  x0, x1;
                f32 u0, u1;
                if (pdfcolumn(u, L+k, b->col, b->ncol, c, &x0, &x1, &u0, &u1)) {
                    pdfstep(&st, L[k].v, L[k].size);
                }
            }
            f32 thr = pdfparathr(&st);
            t->wantblank |= fl.any;
            fl.any = 0;
            for (iz k = b->beg; k < b->end; k++) {
                iz  x0, x1;
                f32 u0, u1;
                if (pdfcolumn(u, L+k, b->col, b->ncol, c, &x0, &x1, &u0, &u1)) {
                    pdfputflow(t, &fl, thr, L[k].v, L[k].size);
                    pdfplacebefore(t, r, pp, L[k].v, u0, u1);
                    pdfputseg(t, r, sg, x0, x1, b->cell, b->ncell);
                }
            }
        }
        t->wantblank = 1;  // the band's end
        fl.any = 0;
        bi++;
    }
    return 1;
}

// A page's text: its lines, images placed before the first line of the
// main direction below their tops that they overlap, the rest after. A
// layout without the room stops the read for memory, and its page is
// not called empty.
static void pdfputpage(PdfText *t, PdfRun *r, Slice<PdfLine> lines)
{
    Arena   *a    = &r->p->page;
    PdfUnit *u    = r->units.data;
    iz       np   = r->places.len;
    iz       most = 0;
    for (iz k = 0; k < lines.len; k++) {
        iz n = lines.data[k].end - lines.data[k].beg;
        most = n>most ? n : most;
    }
    PdfPlacing pp = {};
    pp.order = pdflayoutnew<iz>(a, np);
    pp.done  = pdflayoutnew<u8>(a, np);
    pp.nx    = pdflayoutnew<iz>(a, np);
    PdfSeg sg = {};
    sg.ord  = pdflayoutnew<i32>(a, most);
    sg.ord2 = pdflayoutnew<i32>(a, most);
    sg.atom = pdflayoutnew<i32>(a, most);
    sg.brk  = pdflayoutnew<u8>(a, most);
    sg.brk2 = pdflayoutnew<u8>(a, most);
    PdfBand *bands = pdflayoutnew<PdfBand>(a, lines.len);
    b32 lost = (!lines.data && r->units.len) ||
               (lines.len && (!sg.ord || !sg.ord2 || !sg.atom || !sg.brk || !sg.brk2 || !bands));
    if ((!pp.order || !pp.done || !pp.nx) && np) {
        np   = 0;  // no room even for that: no placeholders
        lost = 1;
    }
    if (lost) {
        pdfstop(r->p, "out of memory");
    }
    for (iz j = 0; j < np; j++) {
        pp.order[j] = j;
        pp.done[j]  = 0;
    }
    for (iz j = 1; j < np; j++) {  // stable, by top, few: insertion
        iz k = pp.order[j];
        iz i = j;
        for (; i>0 && r->places.data[pp.order[i-1]].y1<r->places.data[k].y1; i--) {
            pp.order[i] = pp.order[i-1];
        }
        pp.order[i] = k;
    }
    pp.np   = np;
    pp.head = np ? 0 : -1;
    for (iz j = 0; j < np; j++) {
        pp.nx[j] = j+1<np ? j+1 : -1;
    }
    b32 horiz = 0;
    for (iz i = 0; i < r->units.len; i++) {
        if (!u[i].grp) {
            horiz = u[i].dx>0.9f;
            break;
        }
    }

    iz  before = t->lines;
    b32 cut    = 0;  // lost after some of its text
    for (iz gb = 0; !lost && gb<lines.len;) {
        iz ge = gb;
        for (; ge<lines.len && lines.data[ge].grp==lines.data[gb].grp; ge++) {}
        t->wantblank = gb > 0;
        pp.on = horiz && !lines.data[gb].grp;
        if (!pdfputgroup(t, r, lines.data, gb, ge, &sg, &pp, bands)) {
            pdfstop(r->p, "out of memory");
            lost = 1;
            cut  = t->lines != before;
        }
        gb = ge;
    }
    t->wantblank = 0;
    for (iz j = 0; j < np; j++) {
        if (!pp.done[j]) {
            pdfputplace(t, r, pp.order[j]);
        }
    }
    b32 text = 0;
    for (iz i = 0; i < r->units.len; i++) {
        text |= !(u[i].flags & PDFU_SPACE);
    }
    if (cut) {
        pdfnote(t, "[the rest of this page is not shown: out of memory]");
    } else if (lost) {
        pdfnote(t, "[this page is not shown: out of memory]");
    } else if (t->lines == before) {
        pdfnote(t, np ? Str("[no text on this page, only images]")
                      : Str("[no text on this page]"));
    } else if (!text && np) {
        pdfnote(t, "[no text on this page, only images]");
    }
}

// Extra text: form fields, annotations, the title and the outline

// Clean text s cut to at most max bytes, on a character's boundary.
static Str pdfcututf8(Str s, iz max)
{
    if (s.len <= max) {
        return s;
    }
    iz n = max;
    for (; n>0 && (s[n]&0xc0)==0x80; n--) {}
    return trimright(takehead(s, n));
}

// A string's (or a name's) clean text, at most max bytes, in a: charged
// by its length.
static Str pdfstrtext(Pdf *p, Arena *a, PdfObj o, iz max)
{
    if ((o.kind!=PDFO_STR && o.kind!=PDFO_NAME) || !pdfcharge(p, 1+o.s.len)) {
        return {};
    }
    return pdfcututf8(pdftextstr(a, o.s), max);
}

// A field's value as text: a string or name, a number as written, or
// for a choice its option's text where /Opt pairs it with the value.
static Str pdffieldval(Pdf *p, PdfObj v, PdfObj opt)
{
    if (v.kind==PDFO_INT || v.kind==PDFO_REAL) {
        Buf b = {};
        b.a = &p->op;
        if (!pdfreserve(p, &b, 32)) {
            return {};
        }
        if (v.kind == PDFO_INT) {
            print(&b, v.i);
        } else {
            pdfprintreal(&b, v.r);
        }
        return finish(&b);
    }
    for (i32 i = 0; v.kind==PDFO_STR && opt.kind==PDFO_ARRAY && i<opt.len; i++) {
        PdfObj e = pdfget(p, opt.items[i]);
        if (!pdfcharge(p, 1)) {
            return {};
        } else if (e.kind==PDFO_ARRAY && e.len==2) {
            PdfObj x = pdfget(p, e.items[0]);
            PdfObj d = pdfget(p, e.items[1]);
            if (x.kind==PDFO_STR && d.kind==PDFO_STR && pdfcharge(p, x.s.len) && x.s==v.s) {
                v = d;
                break;
            }
        }
    }
    return pdfstrtext(p, &p->op, v, PDF_FIELDMAX);
}

// A widget's field as a unit at its rect, "[field NAME: VALUE]": its
// name the /T parts up its /Parent chain joined by dots, its value /V,
// inherited, as /FT, /Ff and /Opt are. Hidden widgets, signatures, push
// buttons, buttons not in the state chosen, and empty values are left
// out. Appearance streams are never run, so the value reads once.
static void pdfwidget(PdfRun *r, PdfObj w)
{
    Pdf   *p = r->p;
    PdfObj parts[PDF_FIELDDEPTH];
    i32    np = 0;
    PdfObj v = {}, ft = {}, ff = {}, opt = {};
    PdfObj f = w;
    for (i32 i = 0; i<PDF_FIELDDEPTH && f.kind==PDFO_DICT && pdfcharge(p, 1); i++) {
        PdfObj tt = pdfget(p, f, "T");
        if (tt.kind == PDFO_STR) {
            parts[np++] = tt;
        }
        v   = v.kind   ? v   : pdfget(p, f, "V");
        ft  = ft.kind  ? ft  : pdfget(p, f, "FT");
        ff  = ff.kind  ? ff  : pdfget(p, f, "Ff");
        opt = opt.kind ? opt : pdfget(p, f, "Opt");
        f   = pdfget(p, f, "Parent");
    }
    PdfObj flags = pdfget(p, w, "F");
    if (pdfis(ft, "Sig") || (flags.kind==PDFO_INT && (flags.i & 2))) {
        return;
    } else if (pdfis(ft, "Btn")) {
        PdfObj as = pdfget(p, w, "AS");
        if ((ff.kind==PDFO_INT && (ff.i & (1<<16))) || v.kind!=PDFO_NAME ||
            v.s=="Off" || (as.kind==PDFO_NAME && !(as.s==v.s))) {
            return;
        }
    }

    // Rect, normalized
    PdfObj rc = pdfget(p, w, "Rect");
    f64    q[4] = {};
    if (rc.kind!=PDFO_ARRAY || rc.len!=4) {
        return;
    }
    for (i32 i = 0; i < 4; i++) {
        PdfObj o = pdfget(p, rc.items[i]);
        q[i] = pdfreal(o);
        if ((o.kind!=PDFO_INT && o.kind!=PDFO_REAL) || !pdfsane(q[i])) {
            return;
        }
    }
    f64 x0 = q[0]<q[2] ? q[0] : q[2];
    f64 x1 = q[0]<q[2] ? q[2] : q[0];
    f64 y0 = q[1]<q[3] ? q[1] : q[3];
    f64 h  = (q[1]<q[3] ? q[3] : q[1]) - y0;

    // Its text, decoded in op, then written whole
    Arena mark = pdfmark(p);
    Str   names[PDF_FIELDDEPTH];
    Str   vals[32];
    i32   nv = 0;
    iz    total = 16;
    for (i32 i = 0; i < np; i++) {
        names[i] = pdfstrtext(p, &p->op, parts[i], 100);
        total   += names[i].len + 1;
    }
    if (v.kind == PDFO_ARRAY) {
        for (i32 i = 0; i<v.len && nv<countof(vals); i++) {
            Str s = pdffieldval(p, pdfget(p, v.items[i]), opt);
            if (s.len) {
                vals[nv++] = s;
                total     += s.len + 2;
            }
        }
    } else {
        Str s = pdffieldval(p, v, opt);
        if (s.len) {
            vals[nv++] = s;
            total     += s.len;
        }
    }
    Buf b = {};
    b.a = &p->op;
    if (nv && pdfreserve(p, &b, total)) {
        print(&b, "[field");
        for (i32 i = np-1; i >= 0; i--) {
            print(&b, i==np-1 ? Str(" ") : Str("."));
            print(&b, names[i]);
        }
        print(&b, ": ");
        iz at = b.len;
        for (i32 i = 0; i<nv && b.len-at<PDF_FIELDMAX; i++) {
            if (i) print(&b, ", ");
            print(&b, vals[i]);
        }
        b.len = at + pdfcututf8({b.data+at, b.len-at}, PDF_FIELDMAX).len;
        putbyte(&b, ']');  // within the room reserved
        Str s = finish(&b);
        f64 size = h>0 ? (0.7*h<12 ? 0.7*h : 12) : 10;
        pdfunitstr(r, s, x0, y0 + 0.25*h, x1-x0, size<1 ? 1 : size);
    }
    pdfrewind(p, &mark);
}

// An annotation's text, to follow the page's: "[comment: ...]".
struct PdfNote {
    Str label;
    Str text;
};

// A page's annotations: widgets' fields among its units, the others'
// /Contents into notes, links and popups aside.
static void pdfannots(PdfRun *r, PdfObj page, Slice<PdfNote> *notes)
{
    Pdf   *p  = r->p;
    PdfObj an = pdfget(p, page, "Annots");
    if (an.kind != PDFO_ARRAY) {
        return;
    }
    for (i32 i = 0; i<an.len && i<PDF_ANNOTS && pdfcharge(p, 1); i++) {
        PdfObj o  = pdfget(p, an.items[i]);
        PdfObj st = pdfget(p, o, "Subtype");
        if (o.kind!=PDFO_DICT || st.kind!=PDFO_NAME || st.s=="Link" || st.s=="Popup") {
            continue;
        } else if (st.s == "Widget") {
            pdfwidget(r, o);
            continue;
        }
        PdfNote n = {};
        n.text = pdfstrtext(p, &p->page, pdfget(p, o, "Contents"), PDF_NOTEMAX);
        if (!n.text.len) {
            continue;
        } else if (st.s == "Text") {
            n.label = "comment";
        } else if (st.s == "FreeText") {
            n.label = "text box";
        } else {
            u8 *buf = pdfnew<u8>(p, &p->page, 48);
            if (!buf) {
                return;
            }
            Buf b = {};
            b.data = buf;
            b.cap  = 48;
            b.a    = &p->page;  // never grows: at most 11+24 bytes
            print(&b, "annotation ");
            for (iz j = 0; j<st.s.len && b.len<35; j++) {
                u8 c = st.s[j];
                if ((c>='0' && c<='9') || ((c|32)>='a' && (c|32)<='z')) {
                    putbyte(&b, c);
                }
            }
            n.label = trimright(finish(&b));
        }
        *notes = pdfpush(p, &p->page, *notes, n);
    }
}

// Write a bracketed note, "[label: text]", as text wrapped like any.
static void pdfputnote(PdfText *t, Str label, Str text)
{
    u8  tmp[64];
    Buf b = {};
    b.data = tmp;
    b.cap  = countof(tmp);
    putbyte(&b, '[');
    print(&b, takehead(label, label.len<48 ? label.len : 48));
    putbyte(&b, ':');
    pdfwend(t);
    if (pdfwput(t, finish(&b), 0)) {
        pdfwwords(t, text, 1);
        pdfwput(t, "]", 0);
    }
    pdfwend(t);
}

static b32 pdfu64less(u64 *a, u64 *b)
{
    return *a < *b;
}

// The value of a named destination: from the catalog's /Dests for a
// name, or its /Names /Dests tree, descended by /Limits, for a string.
static PdfObj pdfnamed(Pdf *p, PdfObj key)
{
    PdfObj root = pdfget(p, p->root);
    if (key.kind == PDFO_NAME) {
        PdfObj d = pdfget(p, pdfget(p, root, "Dests"), key.s);
        if (d.kind) {
            return d;
        }
    }
    PdfObj node = pdfget(p, pdfget(p, root, "Names"), "Dests");
    for (i32 depth = 0; depth<PDF_OUTDEPTH && node.kind==PDFO_DICT; depth++) {
        PdfObj names = pdfget(p, node, "Names");
        if (names.kind == PDFO_ARRAY) {
            for (i32 i = 0; i+1<names.len && pdfcharge(p, 1); i += 2) {
                PdfObj k = pdfget(p, names.items[i]);
                if (k.kind==PDFO_STR && k.s==key.s) {
                    return pdfget(p, names.items[i+1]);
                }
            }
            return {};
        }
        PdfObj kids = pdfget(p, node, "Kids");
        PdfObj next = {};
        for (i32 i = 0; kids.kind==PDFO_ARRAY && i<kids.len && !next.kind &&
                        pdfcharge(p, 1); i++) {
            PdfObj kid = pdfget(p, kids.items[i]);
            PdfObj lim = pdfget(p, kid, "Limits");
            if (lim.kind==PDFO_ARRAY && lim.len==2) {
                PdfObj lo = pdfget(p, lim.items[0]);
                PdfObj hi = pdfget(p, lim.items[1]);
                if (lo.kind==PDFO_STR && hi.kind==PDFO_STR &&
                    compare(key.s, lo.s)>=0 && compare(key.s, hi.s)<=0) {
                    next = kid;
                }
            }
        }
        node = next;
    }
    return {};
}

// The page, from 1, an outline item goes to, or 0: its /Dest, or a GoTo
// action's, an array whose first item is a page or its index, maybe by
// name. Pages are found by object number in keys, num<<32 | index.
static i32 pdfdestpage(Pdf *p, PdfObj item, u64 *keys, iz nkeys, iz npages)
{
    PdfObj dest = pdfget(p, item, "Dest");
    if (!dest.kind) {
        PdfObj act = pdfget(p, item, "A");
        if (pdfis(pdfget(p, act, "S"), "GoTo")) {
            dest = pdfget(p, act, "D");
        }
    }
    if (dest.kind==PDFO_STR || dest.kind==PDFO_NAME) {
        dest = pdfnamed(p, dest);
    }
    if (dest.kind == PDFO_DICT) {
        dest = pdfget(p, dest, "D");
    }
    if (dest.kind!=PDFO_ARRAY || !dest.len) {
        return 0;
    }
    PdfObj pg = dest.items[0];
    if (pg.kind == PDFO_REF) {
        iz lo = 0, hi = nkeys;
        while (lo < hi) {
            iz mid = lo + (hi - lo)/2;
            if ((i64)(keys[mid]>>32) < pg.ref.num) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        return lo<nkeys && (i64)(keys[lo]>>32)==pg.ref.num ? (i32)(u32)keys[lo] + 1 : 0;
    } else if (pg.kind==PDFO_INT && pg.i>=0 && pg.i<npages) {
        return (i32)pg.i + 1;
    }
    return 0;
}

// The document's head, before its first page: "[title: ...]" from its
// Info, then "[outline: N entries]" and the entries, a line each,
// indented by level, with their pages. The outline is walked without
// recursion, each object once, at most PDF_OUTLINES entries.
static void pdfhead(PdfText *t, Pdf *p, Slice<PdfPage> pages)
{
    Arena mark  = pdfmark(p);
    Str   title = pdfstrtext(p, &p->op, pdfget(p, p->info, "Title"), PDF_TITLEMAX);
    if (title.len) {
        pdfputnote(t, "title", title);
    }

    PdfObj outl = pdfget(p, pdfget(p, p->root), "Outlines");
    PdfObj node = pdfkey(outl, "First");
    u64   *keys = node.kind ? pdfnew<u64>(p, &p->op, pages.len) : 0;
    struct Entry {
        Str title;
        i32 depth;
        i32 page;
    };
    Entry *es = keys ? pdfnew<Entry>(p, &p->op, PDF_OUTLINES) : 0;
    if (!es) {
        pdfrewind(p, &mark);
        return;
    }
    iz nkeys = 0;
    for (iz i = 0; i < pages.len; i++) {
        if (pages.data[i].num > 0) {
            keys[nkeys++] = (u64)pages.data[i].num<<32 | (u64)i;
        }
    }
    pdfsort(p, &p->op, keys, nkeys, pdfu64less);

    PdfObj stack[PDF_OUTDEPTH];
    i32    depth = 0;
    i32    ne    = 0;
    p->epoch++;
    for (;;) {
        while (!node.kind && depth) {
            node = stack[--depth];
        }
        if (!node.kind || ne>=PDF_OUTLINES || !pdfcharge(p, 1)) {
            break;
        }
        PdfObj o = pdfvisit(p, node) ? pdfget(p, node) : PdfObj{};
        if (o.kind != PDFO_DICT) {
            node = {};
            continue;
        }
        es[ne].title = pdfstrtext(p, &p->op, pdfget(p, o, "Title"), PDF_TITLEMAX);
        es[ne].depth = depth;
        es[ne].page  = pdfdestpage(p, o, keys, nkeys, pages.len);
        ne++;
        PdfObj first = pdfkey(o, "First");
        node = pdfkey(o, "Next");
        if (first.kind && depth<PDF_OUTDEPTH-1) {
            stack[depth++] = node;
            node = first;
        }
    }
    b32 more = ne>=PDF_OUTLINES && (node.kind || depth);

    if (ne) {
        u8  tmp[64];
        Buf b = {};
        b.data = tmp;
        b.cap  = countof(tmp);
        print(&b, "[outline: ");
        print(&b, (i64)ne);
        print(&b, ne==1 ? Str(" entry]") : Str(" entries]"));
        pdfwend(t);
        pdfwput(t, finish(&b), 0);
        pdfwend(t);
    }
    static char const spaces[2*PDF_OUTDEPTH+1] = "                                ";
    for (i32 i = 0; i < ne; i++) {
        pdfwput(t, {(u8 *)spaces, 2*(iz)es[i].depth + 2}, 0);
        pdfwwords(t, es[i].title.len ? es[i].title : Str("(no title)"), 0);
        if (es[i].page) {
            u8  tmp[32];
            Buf b = {};
            b.data = tmp;
            b.cap  = countof(tmp);
            print(&b, "(page ");
            print(&b, (i64)es[i].page);
            putbyte(&b, ')');
            pdfwput(t, finish(&b), 1);
        }
        pdfwend(t);
    }
    if (more) {
        pdfwput(t, "[outline: more entries not shown]", 0);
        pdfwend(t);
    }
    pdfrewind(p, &mark);
}

// Read a page into units and places, then lay it out into t.
static void pdfpage(PdfDoc *d, PdfText *t, PdfPage *pg, i32 pageno)
{
    Pdf   *p    = d->p;
    Arena  mark = p->page;
    PdfRun r    = {};
    r.d       = d;
    r.p       = p;
    r.pageno  = pageno;
    r.stream  = -1;
    r.pool.a  = &p->page;
    r.gs.ctm  = PDF_IDENTITY;
    r.gs.th   = 1;
    r.tm      = r.tlm = PDF_IDENTITY;

    // The MediaBox, normalized, or Letter
    r.box[0] = 0, r.box[1] = 0, r.box[2] = 612, r.box[3] = 792;
    PdfObj mb = pdfget(p, pg->mediabox);
    if (mb.kind==PDFO_ARRAY && mb.len==4) {
        f64 b[4] = {};
        b32 ok   = 1;
        for (i32 i = 0; i < 4; i++) {
            PdfObj o = pdfget(p, mb.items[i]);
            b[i] = pdfreal(o);
            ok  &= (o.kind==PDFO_INT || o.kind==PDFO_REAL) && pdfsane(b[i]);
        }
        if (ok && b[0]!=b[2] && b[1]!=b[3]) {
            r.box[0] = b[0]<b[2] ? b[0] : b[2];
            r.box[2] = b[0]<b[2] ? b[2] : b[0];
            r.box[1] = b[1]<b[3] ? b[1] : b[3];
            r.box[3] = b[1]<b[3] ? b[3] : b[1];
        }
    }

    Str content = pdfcontents(&r, pdfkey(pg->page, "Contents"));
    pdfrun(&r, content, pdfget(p, pg->resources));
    Slice<PdfNote> notes = {};
    pdfannots(&r, pg->page, &notes);

    Slice<PdfLine> lines = pdflines(&r);
    pdfputpage(t, &r, lines);
    for (iz i = 0; i < notes.len; i++) {
        pdfputnote(t, notes.data[i].label, notes.data[i].text);
    }
    if (r.unmapped) {
        u8  tmp[128];
        Buf b = {};
        b.data = tmp;
        b.cap  = countof(tmp);
        b.a    = &p->op;  // never grows
        putbyte(&b, '[');
        print(&b, r.unmapped);
        print(&b, r.unmapped==1 ? Str(" character on this page is")
                                : " characters on this page are");
        print(&b, " in fonts with no Unicode mapping]");
        pdfnote(t, finish(&b));
    }
    if (r.glyphcap) {
        pdfnote(t, "[the rest of this page is not shown: it has too many glyphs]");
    }
    if (r.cut) {
        pdfnote(t, "[the rest of this page is not shown: its content is too long]");
    }
    if (r.placecap) {
        pdfnote(t, "[more images on this page are not noted: it has too many]");
    }

    // Placements kept for the document, as their lines are known now
    for (iz i = 0; i < r.places.len; i++) {
        if (r.places.data[i].line >= 0) {
            d->places = pdfpush(p, &p->keep, d->places, r.places.data[i]);
        }
    }
    p->page = mark;
}

// Copy a slice's items to perm, or null if they do not fit.
template<typename T>
static Slice<T> pdfkeep(Arena *perm, Slice<T> s)
{
    Slice<T> r   = {};
    iz       pad = -(uz)perm->beg & (alignof(T) - 1);
    if (!s.len || !pdffits(perm, pad, s.len, sizeof(T))) {
        return r;
    }
    r.data = alloc<T>(perm, s.len);
    r.len  = r.cap = s.len;
    for (iz i = 0; i < s.len; i++) {
        r.data[i] = s.data[i];
    }
    return r;
}

struct PdfReq {
    PdfLimits lim;  // zero for the defaults
};

struct PdfOut {
    i32             status;     // PDFR_*
    Str             why;        // a refusal's reason, or what cut it short
    i32             pages;      // in the document
    Str             text;
    Slice<iz>       pageline;   // each page read: its marker's line, from 1
    Slice<PdfPlace> places;
    b32             recovered;  // its structure came from a scan
    Str             title;      // from its Info, or null
};

// Read a document's text, every page in order, each starting with a
// "--- page N ---" line, and note where its images are. A refusal
// leaves perm as it was.
static PdfOut pdfread(Arena *perm, Arena scratch, Str data, PdfReq *req)
{
    PdfOut r    = {};
    Arena  save = *perm;
    Pdf    p    = pdfstart(scratch, data, req ? req->lim : PdfLimits{});
    i32    s    = pdfload(&p);
    r.recovered = p.recovered;
    if (s == PDFR_NOTPDF) {
        r.status = s;
        r.why    = "it is not a PDF";
        return r;
    } else if (s == PDFR_ENCRYPTED) {
        r.status = s;
        r.why    = "it is encrypted";
        return r;
    }
    PdfPages g = pdfpages(&p);
    r.recovered = p.recovered;
    r.pages     = (i32)g.pages.len;
    if (!g.pages.len) {
        *perm    = save;
        r.status = PDFR_NOPAGES;
        r.why    = p.why.data ? p.why : Str("it has no pages");
        return r;
    }
    r.title = pdfstrtext(&p, perm, pdfget(&p, p.info, "Title"), PDF_TITLEMAX);

    // Text shown with no font reads as WinAnsi
    PdfFont def = {};
    def.em = 1;
    for (i32 c = 0; c < PDF_FONTCODES; c++) {
        def.uni[c]   = PDF_WINANSI[c];
        def.width[c] = c==32 ? 0.25f : 0.5f;
    }
    PdfDoc d = {};
    d.p       = &p;
    d.deffont = &def;

    PdfText   t     = {};
    Slice<iz> lines = {};
    t.b.a   = perm;
    t.blank = 1;
    pdfhead(&t, &p, g.pages);
    i32 k = 0;
    for (; k<r.pages && !p.why.data && !t.why.data; k++) {
        u8  tmp[32];
        Buf m = {};
        m.data = tmp;
        m.cap  = countof(tmp);
        m.a    = &p.op;  // never grows
        print(&m, "--- page ");
        print(&m, (i64)k + 1);
        print(&m, " ---");
        pdfwend(&t);
        if (!pdftextroom(&t, m.len+1, 0)) {
            break;
        }
        lines = pdfpush(&p, &p.keep, lines, t.lines+1);
        if (p.why.data) {
            break;
        }
        print(&t.b, finish(&m));
        putbyte(&t.b, '\n');
        t.lines++;
        t.ls        = t.b.len;
        t.blank     = 1;
        t.wantblank = 0;
        pdfpage(&d, &t, g.pages.data+k, k+1);
    }
    pdfwend(&t);

    Str why = p.why.data ? p.why : t.why;
    if (why.data) {
        u8  tmp[256];
        Buf m = {};
        m.data = tmp;
        m.cap  = countof(tmp);
        m.a    = &p.op;  // never grows
        print(&m, "[the text stops here: ");
        print(&m, takehead(why, why.len<120 ? why.len : 120));
        if (k < r.pages) {
            print(&m, k+1<r.pages ? Str("; pages ") : "; page ");
            print(&m, (i64)k + 1);
            if (k+1 < r.pages) {
                print(&m, " to ");
                print(&m, (i64)r.pages);
            }
            print(&m, " not read");
        }
        putbyte(&m, ']');
        pdfnote(&t, finish(&m));
    }

    // Results, after the text, which is trimmed to its length
    r.text = finish(&t.b);
    b32 last = t.b.data && (byte *)(t.b.data+t.b.cap)==perm->beg;
    if (last) {
        perm->beg = (byte *)(t.b.data + t.b.len);
    }
    r.pageline = pdfkeep(perm, lines);
    r.places   = pdfkeep(perm, d.places);
    if (r.pageline.len < lines.len) {
        // Markers without their lines would mislead: none at all, and
        // the text given back when it was perm's last
        perm->beg = last ? (byte *)t.b.data : perm->beg;
        r.text   = {};
        r.places = {};
        why      = "out of memory";
    }
    for (iz i = 0; i < r.places.len; i++) {
        PdfPlace *pl = r.places.data + i;
        pl->label = fits(perm, pl->label.len) ? clone(perm, pl->label) : Str{};
    }
    r.status = why.data ? PDFR_PARTIAL : PDFR_OK;
    r.why    = why;
    return r;
}

// What is wrong with a read's results, or null: for tests and the
// fuzzer. A refusal has no text. Text is UTF-8 without controls but
// newline and tab, at most PDF_TEXTMAX, in whole lines; page markers
// run 1..n at the lines pageline names, and no other line is one; each
// placement names a page read and a line in it. Given a scratch to
// hold where lines start, also that the line is its "[image N: ...]".
static Str pdfoutbad(PdfOut *r, Arena *scratch = 0)
{
    if (r->status!=PDFR_OK && r->status!=PDFR_PARTIAL) {
        b32 empty = !r->text.len && !r->pageline.len && !r->places.len;
        return empty && r->why.len ? Str{} : Str("a refusal with results");
    } else if ((r->status==PDFR_PARTIAL) != !!r->why.data) {
        return "partial without why, or why without partial";
    }
    Str s = r->text;
    if (s.len > PDF_TEXTMAX) {
        return "text too long";
    } else if (s.len && s[s.len-1]!='\n') {
        return "a partial last line";
    }
    for (iz i = 0; i < s.len;) {
        i32 c = utf8strict(s, &i);
        if (c<0 || (c>=0xd800 && c<=0xdfff)) {
            return "not UTF-8";
        } else if ((c<0x20 && c!='\n' && c!='\t') || c==0x7f) {
            return "a control";
        }
    }
    iz line = 1;
    iz page = 0;
    for (iz i = 0; i < s.len; line++) {
        iz  j = i;
        for (; s[j] != '\n'; j++) {}
        Str l = {s.data+i, j-i};
        if (startswith(l, "--- page ")) {
            if (page>=r->pageline.len || r->pageline.data[page]!=line) {
                return "a marker not at its pageline";
            }
            Buf b = {};
            u8  tmp[32];
            b.data = tmp;
            b.cap  = countof(tmp);
            print(&b, "--- page ");
            print(&b, page + 1);
            print(&b, " ---");
            if (!(l == finish(&b))) {
                return "a marker out of order";
            }
            page++;
        }
        i = j + 1;
    }
    if (page != r->pageline.len) {
        return "a pageline without its marker";
    } else if (page > r->pages) {
        return "more pages read than there are";
    }
    for (iz i = 0; i < r->places.len; i++) {
        PdfPlace *pl = r->places.data + i;
        if (pl->page<1 || pl->page>page) {
            return "a placement on no page read";
        }
        iz lo = r->pageline.data[pl->page-1];
        iz hi = pl->page<page ? r->pageline.data[pl->page] : line;
        if (pl->line<=lo || pl->line>=hi) {
            return "a placement on no line of its page";
        }
    }
    iz pad = scratch ? -(uz)scratch->beg & (alignof(iz) - 1) : 0;
    if (r->places.len && scratch && pdffits(scratch, pad, line, sizeof(iz))) {
        Arena a     = *scratch;
        iz   *start = alloc<iz>(&a, line);
        iz    n     = 1;
        start[0] = 0;
        for (iz i = 0; i < s.len; i++) {
            if (s[i]=='\n' && i+1<s.len) {
                start[n++] = i + 1;
            }
        }
        for (iz i = 0; i < r->places.len; i++) {
            PdfPlace *pl = r->places.data + i;
            Buf b = {};
            u8  tmp[32];
            b.data = tmp;
            b.cap  = countof(tmp);
            print(&b, "[image ");
            print(&b, (i64)pl->id);
            print(&b, ": ");
            Str l = {s.data+start[pl->line-1], s.len-start[pl->line-1]};
            if (!startswith(l, finish(&b))) {
                return "a placement not on its image's line";
            }
        }
    }
    return {};
}
