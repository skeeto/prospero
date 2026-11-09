// PDF images: samples to RGBA, box-averaged on the way down to a target
// size, so that memory follows the output and not the source; the CCITT
// fax decoder (G3 1-D, G3 2-D and G4); and JPEGs (DCTDecode), passed
// through when they may go as they are, else decoded and converted.
//
// Nothing here knows the parser: plain parameter structs carry what an
// image's dictionary says, and the glue fills them. Nothing calls os_*
// either, so the fuzzer reaches all of it. Every size taken from a file
// is checked against the arena before anything is allocated, and a
// failure leaves the arena as it was, with a reason. Time is bounded by
// a work budget the caller passes in, counted down in units of about 64
// samples or one row.
//
// This is free and unencumbered software released into the public domain.

enum : i32 {
    PDFIMG_EDGE    = 1 << 15,  // columns and rows of an image, at most
    PDFIMG_COMPS   = 32,       // components of a DeviceN, at most (PDF's own)
    PDFIMG_BADROWS = 64,       // G3 rows given up on before decoding stops
    PDFIMG_CONVMEM = 1 << 14,  // tables of one conversion, at most
    PDFIMG_DECODEPIXELS = sizeof(void *)==8 ? 1<<25 : 1<<22,  // JPEG decoded
};


// CCITT fax (ITU-T T.4 and T.6), as CCITTFaxDecode takes it

struct CcittParams {
    i32 k;         // <0 G4, 0 G3 1-D, >0 G3 with a tag bit a line: 1-D or 2-D
    i32 columns;   // 0 for the default, 1728
    i32 rows;      // 0 to decode until the data, or an EOFB, ends
    b32 align;     // EncodedByteAlign
    b32 blackis1;  // BlackIs1: black runs decode to 1 bits, not white runs
};
// EndOfLine and EndOfBlock need no flags: EOLs are taken wherever a line
// may start, and an EOFB (or RTC) ends the image either way.

struct Ccitt {
    Str bits;  // rows of (cols+7)/8 bytes, most significant bit first
    i32 cols;
    i32 rows;
    i32 good;  // leading rows decoded whole, inside the data
    Str err;   // why it stopped short (bits then hold white rows after),
               // or failed (null bits, the arena as it was)
};

// The run-length codes, by run: 0 to 63 then makeup codes 64 to 1728,
// and the extended makeup codes, 1792 to 2560, both colors share.
static char const *const ccwhite[] = {
    "00110101", "000111", "0111", "1000", "1011", "1100", "1110", "1111",
    "10011", "10100", "00111", "01000", "001000", "000011", "110100",
    "110101", "101010", "101011", "0100111", "0001100", "0001000",
    "0010111", "0000011", "0000100", "0101000", "0101011", "0010011",
    "0100100", "0011000", "00000010", "00000011", "00011010", "00011011",
    "00010010", "00010011", "00010100", "00010101", "00010110", "00010111",
    "00101000", "00101001", "00101010", "00101011", "00101100", "00101101",
    "00000100", "00000101", "00001010", "00001011", "01010010", "01010011",
    "01010100", "01010101", "00100100", "00100101", "01011000", "01011001",
    "01011010", "01011011", "01001010", "01001011", "00110010", "00110011",
    "00110100",
    "11011", "10010", "010111", "0110111", "00110110", "00110111",
    "01100100", "01100101", "01101000", "01100111", "011001100",
    "011001101", "011010010", "011010011", "011010100", "011010101",
    "011010110", "011010111", "011011000", "011011001", "011011010",
    "011011011", "010011000", "010011001", "010011010", "011000",
    "010011011",
};
static char const *const ccblack[] = {
    "0000110111", "010", "11", "10", "011", "0011", "0010", "00011",
    "000101", "000100", "0000100", "0000101", "0000111", "00000100",
    "00000111", "000011000", "0000010111", "0000011000", "0000001000",
    "00001100111", "00001101000", "00001101100", "00000110111",
    "00000101000", "00000010111", "00000011000", "000011001010",
    "000011001011", "000011001100", "000011001101", "000001101000",
    "000001101001", "000001101010", "000001101011", "000011010010",
    "000011010011", "000011010100", "000011010101", "000011010110",
    "000011010111", "000001101100", "000001101101", "000011011010",
    "000011011011", "000001010100", "000001010101", "000001010110",
    "000001010111", "000001100100", "000001100101", "000001010010",
    "000001010011", "000000100100", "000000110111", "000000111000",
    "000000100111", "000000101000", "000001011000", "000001011001",
    "000000101011", "000000101100", "000001011010", "000001100110",
    "000001100111",
    "0000001111", "000011001000", "000011001001", "000001011011",
    "000000110011", "000000110100", "000000110101", "0000001101100",
    "0000001101101", "0000001001010", "0000001001011", "0000001001100",
    "0000001001101", "0000001110010", "0000001110011", "0000001110100",
    "0000001110101", "0000001110110", "0000001110111", "0000001010010",
    "0000001010011", "0000001010100", "0000001010101", "0000001011010",
    "0000001011011", "0000001100100", "0000001100101",
};
static char const *const ccext[] = {
    "00000001000", "00000001100", "00000001101", "000000010010",
    "000000010011", "000000010100", "000000010101", "000000010110",
    "000000010111", "000000011100", "000000011101", "000000011110",
    "000000011111",
};

enum : i32 {
    CC_PEEK  = 13,  // bits of the longest run code
    CC_TABLE = 1 << CC_PEEK,
    CC_TEMPS = 2*CC_TABLE*4 + 256,  // both tables, and alignment
};

struct CcBits {
    u8 *data;
    i64 len;  // in bits
    i64 pos;
};

// The next n bits, at most 24, most significant first, zero past the end.
static u32 ccpeek(CcBits *b, i32 n)
{
    i64 at  = b->pos >> 3;
    i64 end = b->len >> 3;
    u32 v   = 0;
    for (i32 i = 0; i < 4; i++) {
        v = v<<8 | (at+i < end ? b->data[at+i] : 0);
    }
    return v << (b->pos & 7) >> (32 - n);
}

// Zero bits from the position, as far as the data goes.
static i64 cczeros(CcBits *b)
{
    i64 p = b->pos;
    for (; p<b->len && !(b->data[p>>3] & (0x80 >> (p&7))); p++) {}
    return p - b->pos;
}

// A lookup of the next CC_PEEK bits: the code's length<<12 | its run, 0
// if no code begins so.
static i32 *cctable(Arena *a, char const *const *codes, iz n)
{
    i32 *t = alloc<i32>(a, CC_TABLE);
    for (iz i = 0; i < n+countof(ccext); i++) {
        char const *s   = i<n ? codes[i] : ccext[i-n];
        i32         run = i<64 ? (i32)i : i<n ? (i32)(i-63)*64 : 1792 + (i32)(i-n)*64;
        i32         len = 0;
        u32         c   = 0;
        for (; s[len]; len++) {
            c = c<<1 | (s[len] == '1');
        }
        u32 lo = c << (CC_PEEK - len);
        u32 hi = (c + 1) << (CC_PEEK - len);
        for (u32 j = lo; j < hi; j++) {
            t[j] = len<<12 | run;
        }
    }
    return t;
}

// One run: makeup codes, then a terminating code. -1 if none decodes.
static i32 ccrun(CcBits *b, i32 *table, i32 cols)
{
    i32 run = 0;
    for (;;) {
        if (b->pos >= b->len) {
            return -1;
        }
        i32 e = table[ccpeek(b, CC_PEEK)];
        if (!e) {
            return -1;
        }
        b->pos += e >> 12;
        run    += e & 4095;
        if ((e & 4095) < 64) {
            return run;
        } else if (run > cols+2560) {
            return -1;  // longer than any line
        }
    }
}

enum : i32 { CC_V0 = 3, CC_P = 7, CC_H, CC_BAD };  // V modes: CC_V0+delta

// The next 2-D mode. Extensions, uncompressed mode and EOLs are bad.
static i32 ccmode(CcBits *b)
{
    static u8 const len[] = {0, 0, 7, 7, 6, 6, 6, 6, 4, 4, 4, 4, 4, 4, 4, 4};
    static u8 const mode[] = {
        CC_BAD, CC_BAD, CC_V0-3, CC_V0+3, CC_V0-2, CC_V0-2, CC_V0+2, CC_V0+2,
        CC_P,   CC_P,   CC_P,    CC_P,    CC_P,    CC_P,    CC_P,    CC_P,
    };
    if (b->pos >= b->len) {
        return CC_BAD;
    }
    u32 v = ccpeek(b, 7);
    if (v >= 0x40) {
        b->pos += 1;
        return CC_V0;
    } else if (v >= 0x10) {
        b->pos += 3;
        return v>=0x30 ? CC_V0+1 : v>=0x20 ? CC_V0-1 : CC_H;
    }
    b->pos += len[v];
    return mode[v];
}

// The changing elements of one 2-D line, decoded against those of the
// line above, ref, into cur. The count, or -1 if it does not decode.
// Each mode must move a0 right, so a line takes at most cols+1.
static iz cc2d(CcBits *b, i32 *wt, i32 *bt, i32 *ref, i32 *cur, i32 cols)
{
    iz  n     = 0;
    iz  i     = 0;   // into ref, which runs up strictly, then cols thrice
    i32 a0    = -1;  // before the first pixel
    i32 color = 0;   // white
    while (a0 < cols) {
        for (; i>0 && ref[i-1]>a0; i--) {}
        for (; ref[i] <= a0; i++) {}
        i += (i & 1) != color;  // b1 changes to the color opposite a0's
        i32 b1 = ref[i];
        i32 b2 = ref[i+1];
        i32 m  = ccmode(b);
        if (m == CC_P) {
            a0 = b2;
        } else if (m == CC_H) {
            i32 r1 = ccrun(b, color ? bt : wt, cols);
            i32 r2 = r1<0 ? -1 : ccrun(b, color ? wt : bt, cols);
            if (r2 < 0) {
                return -1;
            }
            i32 a1 = (a0<0 ? 0 : a0) + r1;
            a1 = a1<cols ? a1 : cols;
            i32 a2 = a1 + r2;
            a2 = a2<cols ? a2 : cols;
            if (a2 <= a0) {
                return -1;
            }
            cur[n++] = a1;
            cur[n++] = a2;
            a0 = a2;
        } else if (m < CC_P) {
            i32 a1 = b1 + m - CC_V0;
            a1 = a1<0 ? 0 : a1>cols ? cols : a1;
            if (a1 <= a0) {
                return -1;
            }
            cur[n++] = a1;
            a0     = a1;
            color ^= 1;
        } else {
            return -1;
        }
    }
    return n;
}

// The changing elements of one 1-D line, or -1.
static iz cc1d(CcBits *b, i32 *wt, i32 *bt, i32 *cur, i32 cols)
{
    iz  n     = 0;
    i32 a0    = 0;
    i32 color = 0;
    while (a0 < cols) {
        if (n > 2*(iz)cols+4) {
            return -1;  // too many empty runs
        }
        i32 r = ccrun(b, color ? bt : wt, cols);
        if (r < 0) {
            return -1;
        }
        a0 = r<cols-a0 ? a0+r : cols;
        cur[n++] = a0;
        color ^= 1;
    }
    return n;
}

// Changing elements as a reference line: those inside the line, strictly
// increasing (an empty run is no change), then cols thrice.
static void ccsettle(i32 *ch, iz n, i32 cols)
{
    iz m = 0;
    for (iz j = 0; j<n && ch[j]<cols; j++) {
        if (m && ch[m-1]==ch[j]) {
            m--;
        } else {
            ch[m++] = ch[j];
        }
    }
    ch[m] = ch[m+1] = ch[m+2] = cols;
}

static void ccrender(u8 *row, iz stride, i32 *ch, i32 cols, b32 blackis1)
{
    __builtin_memset(row, blackis1 ? 0x00 : 0xff, (uz)stride);
    for (iz j = 0; ch[j] < cols; j += 2) {
        for (i32 x = ch[j]; x < ch[j+1]; x++) {
            row[x>>3] ^= (u8)(0x80 >> (x&7));
        }
    }
}

// Move to the next EOL, so that a line start takes it. Whether there is
// one.
static b32 ccresync(CcBits *b, i64 *work)
{
    i64 zeros = 0;
    for (i64 p = b->pos; p < b->len; p++) {
        if (!(p & 63) && --*work<0) {
            return 0;
        }
        if (!(b->data[p>>3] & (0x80 >> (p&7)))) {
            zeros++;
        } else if (zeros >= 11) {
            b->pos = p - 11;
            return 1;
        } else {
            zeros = 0;
        }
    }
    return 0;
}

struct CcRun {
    i32 rows;
    i32 good;
    Str err;
};

// Decode up to maxrows rows into out, or only count them given null.
// Uses CC_TEMPS plus two arrays of 2*cols+16 i32.
static CcRun ccdecode(Arena scratch, Str data, CcittParams p, i32 cols,
                      u8 *out, i32 maxrows, i64 *work)
{
    CcRun r   = {};
    i32  *wt  = cctable(&scratch, ccwhite, countof(ccwhite));
    i32  *bt  = cctable(&scratch, ccblack, countof(ccblack));
    iz    cap = 2*(iz)cols + 16;
    i32  *ref = alloc<i32>(&scratch, cap);
    i32  *cur = alloc<i32>(&scratch, cap);
    ref[0] = ref[1] = ref[2] = cols;  // a white line above the first

    iz     stride = ((iz)cols + 7)/8;
    CcBits b      = {data.data, (i64)data.len*8, 0};
    b32    clean  = 1;  // every row so far decoded inside the data
    i32    bad    = 0;
    i32    y      = 0;
    for (; y < maxrows; y++) {
        *work -= cols/64 + 1;
        if (*work < 0) {
            r.err = "too much work to decode the image";
            break;
        }

        // EOLs, after fill bits; only zeros, an RTC or an EOFB end it
        if (p.k<0 && p.align) {
            b.pos = (b.pos + 7) & ~(i64)7;
        }
        i32 eols = 0;
        for (;;) {
            i64 z = cczeros(&b);
            if (b.pos+z >= b.len) {
                b.pos = b.len;
                break;
            } else if (z < 11) {
                break;
            }
            b.pos += z + 1;
            eols++;
        }
        if (b.pos>=b.len || eols>=2 || (p.k<0 && eols)) {
            break;
        }

        iz n = -1;
        if (p.k < 0) {
            n = cc2d(&b, wt, bt, ref, cur, cols);
        } else {
            if (!eols && p.align) {
                b.pos = (b.pos + 7) & ~(i64)7;
            }
            b32 twod = 0;
            if (p.k > 0) {
                twod = !ccpeek(&b, 1);
                b.pos++;
            }
            n = twod ? cc2d(&b, wt, bt, ref, cur, cols) : cc1d(&b, wt, bt, cur, cols);
        }

        if (n >= 0) {
            ccsettle(cur, n, cols);
            i32 *t = ref;
            ref = cur;
            cur = t;
            clean &= b.pos <= b.len;
            r.good = clean ? y+1 : r.good;
        } else if (p.k < 0) {
            r.err = "bad CCITT data";
            break;  // G4 cannot resync: this row and the rest stay white
        } else {
            clean = 0;  // a bad G3 row repeats the one above, up to an EOL
            if (++bad > PDFIMG_BADROWS) {
                r.err = "too much bad CCITT data";
                break;
            } else if (!ccresync(&b, work)) {
                r.err = "bad CCITT data";
                b.pos = b.len;  // after this row
            }
        }
        if (out) {
            ccrender(out+y*stride, stride, ref, cols, p.blackis1);
        }
    }
    r.rows = y;
    for (i32 i = y; out && i<maxrows; i++) {
        __builtin_memset(out+i*stride, p.blackis1 ? 0x00 : 0xff, (uz)stride);
    }
    return r;
}

// Decode CCITT fax data of at most maxpixels pixels. The rows go in a,
// which is left as it was on failure. Rows the data does not reach are
// white, and err says why they were not decoded.
static Ccitt pdfccitt(Arena *a, Str data, CcittParams p, i64 maxpixels, i64 *work)
{
    Ccitt r    = {};
    i32   cols = p.columns ? p.columns : 1728;
    if (cols<1 || cols>PDFIMG_EDGE || p.rows<0 || p.rows>PDFIMG_EDGE) {
        r.err = "bad CCITT parameters";
        return r;
    }
    iz  stride = ((iz)cols + 7)/8;
    i64 temps  = CC_TEMPS + 2*(2*(i64)cols + 16)*4;
    i32 rows   = p.rows;
    if (!rows) {
        Arena t = *a;
        if (!fitsi64(&t, temps)) {
            r.err = "out of memory to decode the image";
            return r;
        }
        i64   most = maxpixels / cols;
        most = most<PDFIMG_EDGE ? most : (i64)PDFIMG_EDGE;
        CcRun c    = ccdecode(t, data, p, cols, 0, (i32)most, work);
        rows = c.rows;
        if (!rows) {
            r.err = c.err.data ? c.err : Str("no CCITT rows");
            return r;
        }
    }
    if ((i64)rows*cols > maxpixels) {
        r.err = "too many pixels";
        return r;
    } else if (!fitsi64(a, (i64)rows*stride + temps)) {
        r.err = "out of memory to decode the image";
        return r;
    }
    u8   *out = allocbytes(a, rows*stride);
    CcRun c   = ccdecode(*a, data, p, cols, out, rows, work);
    r.bits = {out, rows*stride};
    r.cols = cols;
    r.rows = rows;
    r.good = c.good;
    r.err  = c.err;
    return r;
}


// Samples to RGBA

enum : i32 {
    PDFCS_GRAY,
    PDFCS_RGB,
    PDFCS_CMYK,
    PDFCS_LAB,
    PDFCS_INDEXED,
    PDFCS_TINT,  // Separation or DeviceN: more ink is darker gray
};

// An image as its dictionary describes it, its samples after any filters
// that are not image codecs.
struct PdfSamples {
    Str  data;     // rows of samples, each starting on a byte
    i32  w, h;
    i32  bpc;      // 1, 2, 4, 8 or 16 (its high byte counts)
    i32  cs;       // PDFCS_*
    i32  ncomp;    // TINT's components; the other spaces imply theirs
    i32  base;     // INDEXED: the palette's space, GRAY, RGB, CMYK or LAB
    i32  hival;    // INDEXED: the largest index
    Str  palette;  // INDEXED: entries of the base's components, a byte each
    i32  ndecode;  // values in decode, else the space's default
    f32  decode[2*PDFIMG_COMPS];  // LAB: [0 100 amin amax bmin bmax]
    b32  mask;     // ImageMask: a 1 bpc stencil painting fill
    u8   fill[3];
    i32  nkey;     // /Mask as an array: raw sample ranges left unpainted
    i32  key[2*PDFIMG_COMPS];
    PdfSamples *smask;  // a soft mask, gray at any size, else null
};

struct PdfConv {
    i32  ncomp;
    i32  shift;   // 8 for 16 bpc: the tables take the high byte
    u8  *lut;     // one component: an RGB triple for each value
    u8  *comp;    // more: 256 bytes for each component
    f32 *lab;     // LAB: 256 decoded values for each of L, a and b
    u8  *gamma;   // LAB: linear light to sRGB, in 1024 steps
    i64  stride;  // bytes a row
    Str  err;
};

static i32 pdfcomps(i32 cs)
{
    switch (cs) {
    case PDFCS_GRAY:    return 1;
    case PDFCS_RGB:     return 3;
    case PDFCS_CMYK:    return 4;
    case PDFCS_LAB:     return 3;
    case PDFCS_INDEXED: return 1;
    }
    return 0;
}

// [0,1] as a byte, NaN as 0.
static u8 pdfunit(f32 x)
{
    x = x*255 + 0.5f;
    return x>=255 ? 255 : x>=0 ? (u8)x : 0;
}

// Sample v, of maxv at most, of component c: decoded between lo and hi
// by default.
static f32 pdfdecoded(PdfSamples *s, i32 ncomp, i32 c, i32 v, i32 maxv, f32 lo, f32 hi)
{
    if (s->ndecode >= 2*ncomp) {
        lo = s->decode[2*c];
        hi = s->decode[2*c+1];
    }
    return lo + (f32)v*(hi - lo)/(f32)maxv;
}

static f32 labinv(f32 t)
{
    f32 d = 6.0f/29;
    return t>d ? t*t*t : 3*d*d*(t - 4.0f/29);
}

// CIE L*a*b* (D50 white) to sRGB.
static void labrgb(f32 l, f32 a, f32 b, u8 *gamma, u8 *rgb)
{
    static f32 const m[] = {  // XYZ (D50) to linear sRGB, Bradford adapted
         3.1338561f, -1.6168667f, -0.4906146f,
        -0.9787684f,  1.9161415f,  0.0334540f,
         0.0719453f, -0.2289914f,  1.4052427f,
    };
    f32 fy = (l + 16)/116;
    f32 x  = 0.96422f * labinv(fy + a/500);
    f32 y  = labinv(fy);
    f32 z  = 0.82521f * labinv(fy - b/200);
    for (i32 c = 0; c < 3; c++) {
        f32 v = m[3*c]*x + m[3*c+1]*y + m[3*c+2]*z;
        v = v<1 ? v : 1;
        v = v>0 ? v : 0;  // and NaN
        rgb[c] = gamma[(i32)(v*1023 + 0.5f)];
    }
}

// Linear light to sRGB, without libm: x^(1/2.4) by bisection.
static void srgbtable(u8 *g)
{
    for (i32 i = 0; i < 1024; i++) {
        f64 lin = i / 1023.0;
        f64 v   = 12.92 * lin;
        if (lin > 0.0031308) {
            f64 t  = lin*lin*lin*lin*lin;  // y^12 == lin^5
            f64 lo = 0;
            f64 hi = 1;
            for (i32 k = 0; k < 30; k++) {
                f64 y  = (lo + hi)/2;
                f64 y4 = y*y*y*y;
                if (y4*y4*y4 < t) {
                    lo = y;
                } else {
                    hi = y;
                }
            }
            v = 1.055*lo - 0.055;
        }
        g[i] = (u8)(v*255 + 0.5);
    }
}

// One palette entry, or a pixel's component bytes, to RGB. CMYK is
// naive, without black generation or a profile.
static void pdfentry(i32 cs, u8 *e, u8 *gamma, u8 *rgb)
{
    switch (cs) {
    case PDFCS_GRAY:
        rgb[0] = rgb[1] = rgb[2] = e[0];
        break;
    case PDFCS_RGB:
        rgb[0] = e[0];
        rgb[1] = e[1];
        rgb[2] = e[2];
        break;
    case PDFCS_CMYK:
        for (i32 c = 0; c < 3; c++) {
            rgb[c] = (u8)(((255 - e[c])*(255 - e[3]) + 127)/255);
        }
        break;
    case PDFCS_LAB:  // a palette's, with the default Range, [-100 100]
        labrgb(e[0]*100.0f/255, e[1]*200.0f/255 - 100, e[2]*200.0f/255 - 100, gamma, rgb);
        break;
    }
}

// Tables to convert s's samples, in a: PDFIMG_CONVMEM at most.
static PdfConv pdfconv(Arena *a, PdfSamples *s)
{
    PdfConv r  = {};
    i32     cs = s->mask ? PDFCS_GRAY : s->cs;
    i32     n  = cs==PDFCS_TINT ? s->ncomp : pdfcomps(cs);
    b32     ok = s->bpc==1 || s->bpc==2 || s->bpc==4 || s->bpc==8 || s->bpc==16;
    ok &= n>=1 && n<=PDFIMG_COMPS && cs>=PDFCS_GRAY && cs<=PDFCS_TINT;
    ok &= !s->mask || s->bpc==1;
    ok &= cs!=PDFCS_INDEXED ||
          (s->bpc<=8 && s->hival>=0 && s->base>=PDFCS_GRAY && s->base<=PDFCS_LAB);
    if (!ok) {
        r.err = "an unsupported kind of image";
        return r;
    }
    r.ncomp  = n;
    r.shift  = s->bpc==16 ? 8 : 0;
    r.stride = ((i64)s->w*n*s->bpc + 7)/8;
    i32 maxv = s->bpc==16 ? 255 : (1<<s->bpc) - 1;

    if (cs==PDFCS_LAB || (cs==PDFCS_INDEXED && s->base==PDFCS_LAB)) {
        r.gamma = allocbytes(a, 1024);
        srgbtable(r.gamma);
    }
    if (n==1 && cs!=PDFCS_TINT) {
        r.lut = alloc<u8>(a, 3*256);
    }

    if (s->mask) {
        for (i32 v = 0; v <= maxv; v++) {
            b32 paint = pdfdecoded(s, 1, 0, v, maxv, 0, 1) < 0.5f;
            u8 *p     = r.lut + 3*v;
            p[0] = paint ? s->fill[0] : 255;
            p[1] = paint ? s->fill[1] : 255;
            p[2] = paint ? s->fill[2] : 255;
        }
    } else if (cs == PDFCS_GRAY) {
        for (i32 v = 0; v <= maxv; v++) {
            u8 g = pdfunit(pdfdecoded(s, 1, 0, v, maxv, 0, 1));
            r.lut[3*v] = r.lut[3*v+1] = r.lut[3*v+2] = g;
        }
    } else if (cs == PDFCS_INDEXED) {
        i32 hival = s->hival<255 ? s->hival : 255;
        iz  bn    = s->base==PDFCS_GRAY ? 1 : s->base==PDFCS_CMYK ? 4 : 3;
        for (i32 v = 0; v <= maxv; v++) {
            f32 d    = pdfdecoded(s, 1, 0, v, maxv, 0, (f32)maxv) + 0.5f;
            i32 idx  = d>=(f32)hival ? hival : d>=0 ? (i32)d : 0;
            u8  e[4] = {};
            for (iz c = 0; c < bn; c++) {
                iz at = idx*bn + c;
                e[c]  = at<s->palette.len ? s->palette[at] : 0;
            }
            pdfentry(s->base, e, r.gamma, r.lut+3*v);
        }
    } else if (cs == PDFCS_LAB) {
        r.lab = alloc<f32>(a, 3*256);
        f32 lo[] = {0, -100, -100};
        f32 hi[] = {100, 100, 100};
        for (i32 c = 0; c < 3; c++) {
            for (i32 v = 0; v <= maxv; v++) {
                r.lab[256*c+v] = pdfdecoded(s, 3, c, v, maxv, lo[c], hi[c]);
            }
        }
    } else {
        r.comp = alloc<u8>(a, 256*(iz)n);
        for (i32 c = 0; c < n; c++) {
            for (i32 v = 0; v <= maxv; v++) {
                r.comp[256*c+v] = pdfunit(pdfdecoded(s, n, c, v, maxv, 0, 1));
            }
        }
    }
    return r;
}

// Sample j of a row.
static u32 pdfsample(u8 *row, iz j, i32 bpc)
{
    switch (bpc) {
    case 1:  return row[j>>3] >> (7 - (j&7)) & 1;
    case 2:  return row[j>>2] >> (6 - 2*(j&3)) & 3;
    case 4:  return row[j>>1] >> (4 - 4*(j&1)) & 15;
    case 8:  return row[j];
    }
    return (u32)row[2*j]<<8 | row[2*j+1];
}

// The first n pixels of a row to RGB, and the rest of w white.
static void pdfrow(PdfSamples *s, PdfConv *cv, u8 *row, iz n, u8 *rgb)
{
    i32 nc    = cv->ncomp;
    i32 cs    = s->mask ? PDFCS_GRAY : s->cs;
    b32 haskey = !s->mask && s->nkey>=2*nc;
    for (iz x = 0; x < n; x++) {
        u32 v[PDFIMG_COMPS];
        b32 keyed = haskey;
        for (i32 c = 0; c < nc; c++) {
            v[c]   = pdfsample(row, x*nc+c, s->bpc);
            keyed &= (i64)v[c]>=s->key[2*c] && (i64)v[c]<=s->key[2*c+1];
            v[c] >>= cv->shift;
        }
        u8 *p = rgb + 3*x;
        if (keyed) {
            p[0] = p[1] = p[2] = 255;
        } else if (cv->lut) {
            u8 *e = cv->lut + 3*v[0];
            p[0] = e[0];
            p[1] = e[1];
            p[2] = e[2];
        } else if (cs == PDFCS_LAB) {
            f32 *t = cv->lab;
            labrgb(t[v[0]], t[256+v[1]], t[512+v[2]], cv->gamma, p);
        } else if (cs == PDFCS_TINT) {
            u8 most = 0;
            for (i32 c = 0; c < nc; c++) {
                u8 t = cv->comp[256*c + v[c]];
                most = t>most ? t : most;
            }
            p[0] = p[1] = p[2] = (u8)(255 - most);
        } else {
            u8 e[4];
            for (i32 c = 0; c < nc; c++) {
                e[c] = cv->comp[256*c + v[c]];
            }
            pdfentry(cs, e, 0, p);
        }
    }
    __builtin_memset(rgb+3*n, 255, (uz)(3*(s->w - n)));
}

// Pixels of row y whose samples are all inside the data.
static iz pdfavail(PdfSamples *s, PdfConv *cv, i64 y)
{
    i64 avail = s->data.len - y*cv->stride;
    if (avail <= 0) {
        return 0;
    } else if (avail >= cv->stride) {
        return s->w;
    }
    return (iz)(avail*8 / ((i64)cv->ncomp*s->bpc));
}

// Row sy of a soft mask as alpha, nearest to each pixel of a base row w
// wide.
static void pdfalpha(PdfSamples *m, PdfConv *cv, i64 sy, i32 w, u8 *alpha)
{
    iz  n   = pdfavail(m, cv, sy);
    u8 *row = n ? m->data.data+sy*cv->stride : 0;
    for (i32 x = 0; x < w; x++) {
        iz sx = (iz)((i64)x*m->w/w);
        if (sx < n) {
            u32 v = pdfsample(row, sx, m->bpc) >> cv->shift;
            alpha[x] = cv->lut[3*v];
        } else {
            alpha[x] = 255;  // past the data: the image shows
        }
    }
}

static void pdfover(u8 *rgb, u8 *alpha, i32 w)
{
    for (i32 x = 0; x < w; x++) {
        u32 al = alpha[x];
        for (i32 c = 0; c < 3; c++) {
            u8 *p = rgb + 3*x + c;
            *p = (u8)((*p*al + 255*(255 - al) + 127)/255);
        }
    }
}

// A soft mask that can be used, as gray coverage, else null.
static PdfSamples pdfsmask(PdfSamples *m)
{
    PdfSamples r = {};
    b32 ok = m && m->w>=1 && m->w<=PDFIMG_EDGE && m->h>=1 && m->h<=PDFIMG_EDGE &&
             !m->mask && (m->cs==PDFCS_GRAY || (m->cs==PDFCS_TINT && m->ncomp==1));
    if (ok) {
        r       = *m;
        r.cs    = PDFCS_GRAY;  // a tint's coverage is not darkness here
        r.smask = 0;
        r.nkey  = 0;
    }
    return r;
}

// An image's samples as tw by th RGBA, each pixel the average of its box
// of samples, composited over white, so opaque. Rows past the data are
// white. The pixels go in a, which is left as it was on failure. Memory
// follows the output: one source row and some tables besides the pixels.
static Image pdfpixels(Arena *a, PdfSamples *s, i32 tw, i32 th, i64 *work)
{
    Image r = {};
    i32   w = s->w;
    i32   h = s->h;
    if (w<1 || w>PDFIMG_EDGE || h<1 || h>PDFIMG_EDGE) {
        r.err = "a bad size";
        return r;
    } else if (tw<1 || tw>w || th<1 || th>h) {
        r.err = "a bad size to show it at";
        return r;
    }
    PdfSamples m = pdfsmask(s->mask ? 0 : s->smask);  // as viewers do, an
                                                      // unusable one is none
    i64 out  = (i64)tw*th*4;
    i64 cost = out + (i64)tw*3*8 + ((i64)tw+1)*4 + (i64)w*4 + 2*PDFIMG_CONVMEM + 256;
    if (!fitsi64(a, cost)) {
        r.err = "out of memory to decode the image";
        return r;
    }
    Arena   save = *a;
    u8     *px   = allocbytes(a, (iz)out);
    Arena   t    = *a;
    PdfConv cv   = pdfconv(&t, s);
    if (cv.err.data) {
        *a    = save;
        r.err = cv.err;
        return r;
    }
    PdfConv mv = {};
    if (m.w) {
        mv  = pdfconv(&t, &m);
        m.w = mv.err.data ? 0 : m.w;
    }
    u64 *sums  = alloc<u64>(&t, (iz)tw*3);
    i32 *xs    = alloc<i32>(&t, (iz)tw+1);
    u8  *rgb   = allocbytes(&t, (iz)w*3);
    u8  *alpha = allocbytes(&t, w);
    i64  sy    = -1;
    for (i32 tx = 0; tx <= tw; tx++) {
        xs[tx] = (i32)((i64)tx*w/tw);
    }

    for (i32 ty = 0; ty < th; ty++) {
        i32 y0    = (i32)((i64)ty*h/th);
        i32 y1    = (i32)((i64)(ty+1)*h/th);
        i64 white = 0;  // rows past the data
        __builtin_memset(sums, 0, (uz)tw*3*sizeof(u64));
        for (i32 y = y0; y < y1; y++) {
            iz n = pdfavail(s, &cv, y);
            *work -= n ? (i64)w*cv.ncomp/64 + 1 : tw/64 + 1;
            if (*work < 0) {
                *a    = save;
                r.err = "too much work to decode the image";
                return r;
            } else if (!n) {
                white++;
                continue;
            }
            pdfrow(s, &cv, s->data.data+y*cv.stride, n, rgb);
            if (m.w) {
                i64 my = (i64)y*m.h/h;
                if (my != sy) {
                    sy = my;
                    pdfalpha(&m, &mv, my, w, alpha);
                    *work -= w/64;
                }
                pdfover(rgb, alpha, w);
            }
            for (i32 tx = 0; tx < tw; tx++) {
                u64 *sum = sums + 3*tx;
                for (i32 x = xs[tx]; x < xs[tx+1]; x++) {
                    sum[0] += rgb[3*x+0];
                    sum[1] += rgb[3*x+1];
                    sum[2] += rgb[3*x+2];
                }
            }
        }
        for (i32 tx = 0; tx < tw; tx++) {
            u64 *sum = sums + 3*tx;
            u64  bw  = (u64)(xs[tx+1] - xs[tx]);
            u64  n   = (u64)(y1 - y0) * bw;
            u64  pad = (u64)white * bw * 255;
            u8  *p   = px + ((iz)ty*tw + tx)*4;
            p[0] = (u8)((sum[0] + pad + n/2)/n);
            p[1] = (u8)((sum[1] + pad + n/2)/n);
            p[2] = (u8)((sum[2] + pad + n/2)/n);
            p[3] = 255;
        }
    }
    r.rgba   = px;
    r.width  = tw;
    r.height = th;
    return r;
}


// Showing an image: as a JPEG passed through, or as a PNG

struct PdfShown {
    Str data;  // null on failure
    Str mime;
    Str err;
    i32 w, h;
};

struct PdfSize {
    i32 w, h;
};

// The size w by h comes to within edge pixels on each side and pixels
// in all, as pngfit makes it.
static PdfSize pdffit(i32 w, i32 h, i64 edge, i64 pixels)
{
    PdfSize r = {};
    f64     s = fitscale(w, h, edge, pixels);
    r.w = (i32)(w*s);
    r.h = (i32)(h*s);
    r.w = r.w<1 ? 1 : r.w;
    r.h = r.h<1 ? 1 : r.h;
    return r;
}

static PdfShown pdfshown(Converted c)
{
    PdfShown r = {};
    r.data = c.png;
    r.mime = c.png.data ? Str("image/png") : Str{};
    r.err  = c.err;
    r.w    = c.w;
    r.h    = c.h;
    return r;
}

// An image's samples (raw, or CCITT's bits) shown as a PNG within edge,
// pixels and maxb64, in a, through mem, another arena.
static PdfShown pdfshowsamples(Arena *a, Arena mem, PdfSamples *s,
                               i64 edge, i64 pixels, iz maxb64, i64 *work)
{
    PdfShown r = {};
    if (s->w<1 || s->w>PDFIMG_EDGE || s->h<1 || s->h>PDFIMG_EDGE) {
        r.err = "a bad size";
        return r;
    }
    PdfSize z = pdffit(s->w, s->h, edge, pixels);
    Image   p = pdfpixels(&mem, s, z.w, z.h, work);
    if (p.err.data) {
        r.err = p.err;
        return r;
    }
    return pdfshown(pngfit(a, mem, p.rgba, p.width, p.height, edge, pixels, maxb64));
}

// What a JPEG's markers say: its size and components from its frame
// header, and the transform of an Adobe APP14 marker.
struct PdfJpeg {
    i32 w, h;
    i32 ncomp;
    i32 adobe;  // the APP14 transform plus 1, 0 without the marker
};

static PdfJpeg pdfjpeg(Str d)
{
    PdfJpeg r = {};
    if (d.len<4 || d[0]!=0xff || d[1]!=0xd8) {
        return r;
    }
    for (iz i = 2; i+4 <= d.len;) {
        if (d[i] != 0xff) {
            return r;
        }
        u8 m = d[i+1];
        if (m==0xff || m==0x01 || (m>=0xd0 && m<=0xd8)) {
            i += m==0xff ? 1 : 2;  // fill, or a marker without a length
            continue;
        }
        iz len = d[i+2]<<8 | d[i+3];
        if (len<2 || len>d.len-i-2) {
            return r;
        }
        Str seg = {d.data+i+4, len-2};
        if (m==0xee && seg.len>=12 && startswith(seg, "Adobe")) {
            r.adobe = seg[11] + 1;
        } else if (m>=0xc0 && m<=0xcf && m!=0xc4 && m!=0xc8 && m!=0xcc && seg.len>=6) {
            r.h     = seg[1]<<8 | seg[2];
            r.w     = seg[3]<<8 | seg[4];
            r.ncomp = seg[5];
        } else if (m == 0xda) {
            break;
        }
        i += 2 + len;
    }
    return r;
}

// 1 if Decode is absent or the identity, -1 if every component's is
// inverted, [1 0], else 0.
static i32 pdfplaindecode(PdfSamples *s, i32 n)
{
    if (s->ndecode < 2*n) {
        return 1;
    }
    b32 same = 1;
    b32 inv  = 1;
    for (i32 c = 0; c < n; c++) {
        f32 lo = s->decode[2*c];
        f32 hi = s->decode[2*c+1];
        same &= lo==0 && hi==1;
        inv  &= lo==1 && hi==0;
    }
    return same ? 1 : inv ? -1 : 0;
}

// A DCTDecode image shown: its JPEG passed through when it has 1 or 3
// components, as its dictionary says too, no Decode or masks, and fits
// edge, pixels and maxb64 as it is; else decoded, of PDFIMG_DECODEPIXELS
// at most, and shown as a PNG. The result goes in a, the pixels and the
// rest in mem, another arena.
static PdfShown pdfshowjpeg(Arena *a, Arena mem, PdfSamples *s,
                            i64 edge, i64 pixels, iz maxb64)
{
    PdfShown  r     = {};
    Str       data  = s->data;
    i32       n     = pdfjpeg(data).ncomp;
    ImageKind k     = imagekind(mem, data);
    i32       plain = pdfplaindecode(s, n==4 ? 4 : n==3 ? 3 : 1);
    b32       pass  = k.kind==IMAGE_JPEG && k.w==s->w && k.h==s->h &&
                      !s->mask && !s->smask && !s->nkey && plain==1 &&
                      ((n==1 && s->cs==PDFCS_GRAY) || (n==3 && s->cs==PDFCS_RGB));
    pass &= k.w<=edge && k.h<=edge && (i64)k.w*k.h<=pixels;
    pass &= base64len(data.len) <= maxb64;
    if (pass) {
        // Decoded once, as a check: a JPEG an API would refuse is never
        // sent as it is (its error then comes from decoding it below)
        Arena t = mem;
        Image v = imgdecode(&t, data, pixels);
        pass = v.rgba && v.width==k.w && v.height==k.h;
    }
    if (pass) {
        if (!fits(a, data.len)) {
            r.err = "out of memory to show the image";
            return r;
        }
        r.data = clone(a, data);
        r.mime = "image/jpeg";
        r.w    = k.w;
        r.h    = k.h;
        return r;
    } else if (s->mask) {
        r.err = "an unsupported kind of image";
        return r;
    }

    Image p = imgdecode(&mem, data, PDFIMG_DECODEPIXELS);
    if (p.err.data) {
        r.err = p.err;
        return r;
    }
    i32 w  = p.width;
    i32 h  = p.height;
    u8 *px = p.rgba;

    // Wuffs undoes an Adobe CMYK JPEG's inversion of its own, and the
    // Decode [1 0 1 0 1 0 1 0] Ghostscript gives such a JPEG says the
    // same again, so a four-component JPEG's Decode is not applied, as
    // Poppler does not. A gray or RGB one's inverted Decode is applied,
    // and a Separation's gray is ink, inverted too.
    b32 invert = (n!=4 && plain==-1) ^ (s->cs==PDFCS_TINT);
    for (iz i = 0; invert && i<(iz)w*h; i++) {
        px[4*i+0] = (u8)(255 - px[4*i+0]);
        px[4*i+1] = (u8)(255 - px[4*i+1]);
        px[4*i+2] = (u8)(255 - px[4*i+2]);
    }

    PdfSamples m = pdfsmask(s->smask);
    if (m.w) {
        Arena t = mem;
        if (!fitsi64(&t, PDFIMG_CONVMEM + (i64)w*4 + 64)) {
            r.err = "out of memory to decode the image";
            return r;
        }
        PdfConv mv    = pdfconv(&t, &m);
        u8     *alpha = allocbytes(&t, w);
        u8     *rgb   = allocbytes(&t, (iz)w*3);
        for (i32 y = 0; !mv.err.data && y<h; y++) {
            u8 *row = px + (iz)y*w*4;
            pdfalpha(&m, &mv, (i64)y*m.h/h, w, alpha);
            for (i32 x = 0; x < w; x++) {
                copybytes(rgb+3*x, row+4*x, 3);
            }
            pdfover(rgb, alpha, w);
            for (i32 x = 0; x < w; x++) {
                copybytes(row+4*x, rgb+3*x, 3);
            }
        }
    }
    for (iz i = 0; i < (iz)w*h; i++) {
        px[4*i+3] = 255;
    }
    return pdfshown(pngfit(a, mem, px, w, h, edge, pixels, maxb64));
}


// Images in a document: decoded by placement, and picked for a window
//
// pdfread notes where each image is drawn (PdfPlace) without decoding
// any. pdfimage decodes one later: it loads only the document's xref,
// resolves the image's object, or for an inline image decodes the one
// content stream holding it and lexes from the BI it noted, and shows
// it within the limits given. pdfpick chooses which a window of the
// text shows, and the limits of each.
//
//   PdfPick  k = pdfpick(scratch, out.places, first, last, edge, pixels, maxb64);
//   PdfShown s = pdfimage(&perm, scratch, data, out.places.data+k.at[i],
//                         k.edge, k.pixels, k.maxb64);

enum : i32 {
    PDF_IMAGES     = 8,         // images a window shows, at most
    PDF_SHOWPIXELS = 4 << 20,   // pixels of them all, shared
    PDF_SHOWBYTES  = 10 << 20,  // their base64, in all: within PDF_REQIMGBYTES
    PDF_PICKMIN    = 24,        // pixels on a side, at least, to be shown
    PDF_CSDEPTH    = 4,         // colour spaces nested, as ICC alternates
};

// Bytes per row times rows of an unfiltered inline image, or -1.
static iz pdfinlinelen(Pdf *p, PdfObj dict)
{
    if (pdfget(p, dict, "F").kind || pdfget(p, dict, "Filter").kind) {
        return -1;
    }
    i64 w   = pdfint(p, dict, "W", pdfint(p, dict, "Width", 0));
    i64 h   = pdfint(p, dict, "H", pdfint(p, dict, "Height", 0));
    PdfObj im = pdfget(p, dict, "IM");
    im = im.kind ? im : pdfget(p, dict, "ImageMask");
    b32 mask = im.kind==PDFO_BOOL && im.b;
    i64 bpc = mask ? 1 : pdfint(p, dict, "BPC", pdfint(p, dict, "BitsPerComponent", 8));
    PdfObj cs = pdfget(p, dict, "CS");
    cs = cs.kind ? cs : pdfget(p, dict, "ColorSpace");
    i64 nc = -1;
    if (mask) {
        nc = 1;
    } else if (cs.kind == PDFO_NAME) {
        Str n = cs.s;
        nc = n=="G" || n=="DeviceGray" || n=="I" || n=="Indexed" ? 1
           : n=="RGB" || n=="DeviceRGB" ? 3
           : n=="CMYK" || n=="DeviceCMYK" ? 4 : -1;
    } else if (cs.kind==PDFO_ARRAY && cs.len && pdfis(pdfget(p, cs.items[0]), "Indexed")) {
        nc = 1;
    } else if (cs.kind==PDFO_ARRAY && cs.len && pdfis(pdfget(p, cs.items[0]), "I")) {
        nc = 1;
    }
    if (w<=0 || h<=0 || w>PDF_IMGEDGE || h>PDF_IMGEDGE || nc<0 ||
        (bpc!=1 && bpc!=2 && bpc!=4 && bpc!=8 && bpc!=16)) {
        return -1;
    }
    i64 n = (w*nc*bpc + 7)/8 * h;  // past 32 bits for a 32768 square
    return n<=PDF_STREAMMAX ? (iz)n : -1;
}

// An inline image, lexed from just after its BI: its dict, its items in
// op, and its data, from after ID (and one space) to before EI. The
// data's extent comes from its size when unfiltered and EI follows
// there, else from the first EI between spaces or delimiters.
struct PdfInline {
    PdfObj dict;
    Str    data;
    b32    ok;  // ID was found, and the scan for EI charged
};

static PdfInline pdfinlineat(Pdf *p, PdfLex *x)
{
    PdfInline     r     = {};
    Str           d     = x->d;
    Slice<PdfObj> items = {};
    for (;;) {
        PdfTok t = pdflex(x);
        if (!t.kind || (t.kind==PDFT_KEY && t.s=="ID")) {
            if (!t.kind) {
                return r;
            }
            break;
        } else if (t.kind != PDFT_NAME) {
            continue;
        }
        PdfObj key = {};
        key.kind = PDFO_NAME;
        key.s    = t.s;
        PdfTok vt = pdflex(x);
        if (vt.kind==PDFT_KEY && vt.s=="ID") {
            break;
        }
        PdfObj v = pdfparsetok(x, vt, 1);
        if (items.len < 2*PDF_OPERANDS) {
            items = pdfpush(p, &p->op, items, key);
            items = pdfpush(p, &p->op, items, v);
        }
    }
    r.dict.kind  = PDFO_DICT;
    r.dict.len   = (i32)(items.len/2);
    r.dict.items = items.data;

    iz i = x->i;
    i += i<d.len && pdfspace(d[i]);
    iz n   = pdfinlinelen(p, r.dict);
    iz end = -1;  // where EI starts
    if (n>=0 && n<=d.len-i) {
        iz j = i + n;
        for (; j<d.len && pdfspace(d[j]); j++) {}
        if (j+1<d.len && d[j]=='E' && d[j+1]=='I' && (j+2==d.len || pdfdelim(d[j+2]))) {
            end = j;
        }
    }
    for (iz j = i; end<0 && j+1<d.len; j++) {
        if (d[j]=='E' && d[j+1]=='I' && (j==i || pdfspace(d[j-1])) &&
            (j+2==d.len || pdfspace(d[j+2]) || pdfdelim(d[j+2]))) {
            end = j;
        }
    }
    x->i   = end<0 ? d.len : end+2;
    r.data = {d.data+i, (end<0 ? d.len : end) - i};
    r.ok   = pdfcharge(p, (x->i - i)/64);
    return r;
}

// The numbers of array a, at most n, into out: how many, 0 if it is not
// an array of numbers alone.
static i32 pdfnums(Pdf *p, PdfObj a, f32 *out, i32 n)
{
    a = pdfget(p, a);
    if (a.kind != PDFO_ARRAY) {
        return 0;
    }
    i32 k = 0;
    for (; k<a.len && k<n; k++) {
        PdfObj v = pdfget(p, a.items[k]);
        if (v.kind == PDFO_INT) {
            out[k] = (f32)v.i;
        } else if (v.kind==PDFO_REAL && v.r==v.r) {
            out[k] = (f32)(v.r<-1e9 ? -1e9 : v.r>1e9 ? 1e9 : v.r);
        } else {
            return 0;
        }
    }
    return k;
}

// A colour space into s: its kind and components, and for Indexed its
// base and palette, the palette's stream decoded in page. A name that
// is no device space's is looked up in res's /ColorSpace, at the top
// only. Lab's /Range goes in range ([amin amax bmin bmax]). Whether it
// is a space that can be shown.
static b32 pdfcspace(Pdf *p, PdfObj cs, PdfObj res, PdfSamples *s, f32 *range,
                     b32 base, i32 depth)
{
    cs = pdfget(p, cs);
    if (depth > PDF_CSDEPTH) {
        return 0;
    } else if (cs.kind==PDFO_ARRAY && cs.len==1) {
        cs = pdfget(p, cs.items[0]);  // [/DeviceRGB]
    }
    Str    n  = {};
    PdfObj a1 = {};
    if (cs.kind == PDFO_NAME) {
        n = cs.s;
    } else if (cs.kind==PDFO_ARRAY && cs.len) {
        PdfObj f = pdfget(p, cs.items[0]);
        n  = f.kind==PDFO_NAME ? f.s : Str{};
        a1 = pdfget(p, cs.items[1]);
    }
    if (n=="DeviceGray" || n=="G" || n=="CalGray") {
        s->cs = PDFCS_GRAY;
        return 1;
    } else if (n=="DeviceRGB" || n=="RGB" || n=="CalRGB") {
        s->cs = PDFCS_RGB;
        return 1;
    } else if (n=="DeviceCMYK" || n=="CMYK") {
        s->cs = PDFCS_CMYK;
        return 1;
    } else if (n == "Lab") {
        s->cs = PDFCS_LAB;
        f32 rg[4];
        if (pdfnums(p, pdfget(p, a1, "Range"), rg, 4) == 4) {
            copybytes(range, rg, sizeof(rg));
        }
        return 1;
    } else if (n=="ICCBased" && cs.kind==PDFO_ARRAY) {
        switch (pdfint(p, a1, "N", 0)) {
        case 1: s->cs = PDFCS_GRAY; return 1;
        case 3: s->cs = PDFCS_RGB;  return 1;
        case 4: s->cs = PDFCS_CMYK; return 1;
        }
        return pdfcspace(p, pdfget(p, a1, "Alternate"), {}, s, range, base, depth+1);
    } else if (n=="Separation" && cs.kind==PDFO_ARRAY && !base) {
        s->cs    = PDFCS_TINT;
        s->ncomp = 1;
        return 1;
    } else if (n=="DeviceN" && cs.kind==PDFO_ARRAY && !base) {
        s->cs    = PDFCS_TINT;
        s->ncomp = a1.kind==PDFO_ARRAY ? a1.len : 0;
        return s->ncomp>=1 && s->ncomp<=PDFIMG_COMPS;
    } else if ((n=="Indexed" || n=="I") && cs.kind==PDFO_ARRAY && cs.len>=4 && !base) {
        PdfSamples b     = {};
        f32        rg[4] = {};
        if (!pdfcspace(p, cs.items[1], res, &b, rg, 1, depth+1) || b.cs>PDFCS_LAB) {
            return 0;
        }
        PdfObj hv = pdfget(p, cs.items[2]);
        PdfObj lk = pdfget(p, cs.items[3]);
        if (hv.kind!=PDFO_INT && hv.kind!=PDFO_REAL) {
            return 0;
        }
        i64 hival = hv.kind==PDFO_INT ? hv.i : pdftoint(hv.r);
        s->cs    = PDFCS_INDEXED;
        s->base  = b.cs;
        s->hival = (i32)pdfclamp(hival, 0, 255);
        if (lk.kind == PDFO_STR) {
            s->palette = lk.s;
        } else if (lk.kind == PDFO_STREAM) {
            PdfData d = pdfstream(p, &p->page, lk, 4*256);
            s->palette = d.image ? Str{} : d.data;
        } else {
            return 0;
        }
        return 1;
    } else if (cs.kind==PDFO_NAME && res.kind && !depth) {
        PdfObj v = pdfget(p, pdfget(p, res, "ColorSpace"), n);
        return v.kind && pdfcspace(p, v, {}, s, range, base, depth+1);
    }
    return 0;
}

// Bytes of s's samples, rows starting on a byte, at most PDF_STREAMMAX.
static iz pdfsamplebytes(PdfSamples *s)
{
    i64 n   = s->mask ? 1 : s->cs==PDFCS_TINT ? s->ncomp : pdfcomps(s->cs);
    i64 bpc = s->bpc>=1 && s->bpc<=16 ? s->bpc : 16;
    i64 len = ((i64)s->w*n*bpc + 7)/8 * s->h;
    return (iz)(len<1 ? 1 : len>PDF_STREAMMAX ? PDF_STREAMMAX : len);
}

// What an image's dict says of its samples, into s (its data aside),
// or why it cannot be shown. A colour space name is looked up in res.
// A JPEG's dict may leave its space out, for its own: cs is then -1.
static Str pdfsamplesof(Pdf *p, PdfObj dict, PdfObj res, i32 image, PdfSamples *s)
{
    i64 w = pdfint(p, dict, "Width", -1);
    i64 h = pdfint(p, dict, "Height", -1);
    w = w>=0 ? w : pdfint(p, dict, "W", 0);
    h = h>=0 ? h : pdfint(p, dict, "H", 0);
    if (w<1 || w>PDFIMG_EDGE || h<1 || h>PDFIMG_EDGE) {
        return "a bad size";
    }
    PdfObj im = pdfget(p, dict, "ImageMask");
    im = im.kind ? im : pdfget(p, dict, "IM");
    i64 bpc = pdfint(p, dict, "BitsPerComponent", -1);
    bpc = bpc>=0 ? bpc : pdfint(p, dict, "BPC", 8);
    s->w    = (i32)w;
    s->h    = (i32)h;
    s->mask = im.kind==PDFO_BOOL && im.b;
    s->bpc  = s->mask || image==PDFI_CCITT ? 1 : image==PDFI_DCT ? 8 : (i32)pdfclamp(bpc, 0, 64);

    f32 range[4] = {-100, 100, -100, 100};
    if (s->mask) {
        s->cs = PDFCS_GRAY;
    } else {
        PdfObj cs = pdfget(p, dict, "ColorSpace");
        cs = cs.kind ? cs : pdfget(p, dict, "CS");
        if (!cs.kind && image==PDFI_DCT) {
            s->cs = -1;
        } else if (!pdfcspace(p, cs, res, s, range, 0, 0)) {
            return "an unsupported colour space";
        }
    }

    PdfObj dec = pdfget(p, dict, "Decode");
    dec = dec.kind ? dec : pdfget(p, dict, "D");
    s->ndecode = pdfnums(p, dec, s->decode, 2*PDFIMG_COMPS);
    if (s->cs==PDFCS_LAB && s->ndecode<6) {
        f32 lab[] = {0, 100, range[0], range[1], range[2], range[3]};
        copybytes(s->decode, lab, sizeof(lab));
        s->ndecode = 6;
    }

    // A colour key: sample ranges left unpainted
    PdfObj key = pdfget(p, dict, "Mask");
    f32    k[2*PDFIMG_COMPS];
    i32    nk  = s->mask ? 0 : pdfnums(p, key, k, 2*PDFIMG_COMPS);
    for (i32 i = 0; i < nk; i++) {
        s->key[i] = (i32)(k[i]<-1 ? -1 : k[i]>65535 ? 65535 : k[i]);
    }
    s->nkey = nk;
    return {};
}

// The soft mask of an image's dict, or its /Mask stencil as one, its
// samples decoded in page into m: or false, for none to use.
static b32 pdfsoftmask(Pdf *p, PdfObj dict, PdfSamples *m)
{
    PdfObj sm      = pdfget(p, dict, "SMask");
    b32    stencil = 0;
    if (sm.kind != PDFO_STREAM) {
        sm      = pdfget(p, dict, "Mask");
        stencil = 1;
    }
    if (sm.kind != PDFO_STREAM) {
        return 0;
    } else if (pdfsamplesof(p, sm.st->dict, {}, PDFI_NONE, m).data) {
        return 0;
    } else if (stencil) {
        // As coverage: a stencil's 0 paints, unless its Decode is [1 0]
        if (!m->mask) {
            return 0;
        }
        b32 inv = m->ndecode>=2 && m->decode[0]>m->decode[1];
        m->mask      = 0;
        m->cs        = PDFCS_GRAY;
        m->decode[0] = inv ? 0.0f : 1.0f;
        m->decode[1] = inv ? 1.0f : 0.0f;
        m->ndecode   = 2;
    }
    iz      n = pdfsamplebytes(m);
    p->decoded += n + n/2;  // the budget grows by what the mask holds (pdfimage)
    PdfData d = pdfdecode(p, &p->page, sm.st->dict, sm.st->raw, n);
    if (!d.data.data || d.image) {
        return 0;  // a coded soft mask (JPEG, fax) is not used
    }
    m->data = d.data;
    return 1;
}

// The resources of page n (from 1), for an inline image's colour space.
static PdfObj pdfpageres(Pdf *p, i32 n)
{
    PdfPages g = pdfpages(p);
    return n>=1 && n<=g.pages.len ? pdfget(p, g.pages.data[n-1].resources) : PdfObj{};
}

// Show the image a placement names, within edge pixels on a side,
// pixels in all and maxb64 bytes of base64: a JPEG as it is when it may
// go so, else a PNG. The result goes in perm, which a failure leaves as
// it was, with why. Only the xref is loaded, and only the image's own
// object (with its soft mask) or its content stream is decoded.
static PdfShown pdfimage(Arena *perm, Arena scratch, Str data, PdfPlace *pl,
                         i64 edge, i64 pixels, iz maxb64)
{
    PdfShown r    = {};
    Arena    save = *perm;
    if (pl->image==PDFI_JPX || pl->image==PDFI_JBIG2) {
        r.err = pl->image==PDFI_JPX ? Str("JPEG 2000 images are not shown")
                                    : Str("JBIG2 images are not shown");
        return r;
    }
    Pdf p = pdfstart(scratch, data, {});
    i32 s = pdfload(&p);
    if (s == PDFR_NOTPDF) {
        r.err = "it is not a PDF";
        return r;
    } else if (s == PDFR_ENCRYPTED) {
        r.err = "it is encrypted";
        return r;
    }

    PdfObj ref = {};
    ref.kind    = PDFO_REF;
    ref.ref.num = pl->objnum;
    ref.ref.gen = pl->gen;
    PdfObj o    = pl->objnum>0 ? pdfget(&p, ref) : PdfObj{};
    if (o.kind != PDFO_STREAM) {
        r.err = p.why.data ? p.why : Str("its object is not in the file");
        return r;
    }
    PdfObj dict = o.st->dict;
    Str    raw  = o.st->raw;
    PdfObj res  = {};
    if (pl->inlineoff >= 0) {
        // Its content stream, lexed from the BI noted
        PdfData c = pdfstream(&p, &p.page, o, 0);
        if (!c.data.data || c.image || pl->inlineoff>=c.data.len) {
            r.err = p.why.data ? p.why : Str("its content stream does not hold it");
            return r;
        }
        PdfLex    x  = {&p, &p.op, c.data, pl->inlineoff, 0, 0};
        PdfTok    t  = pdflex(&x);
        PdfInline in = {};
        if (t.kind==PDFT_KEY && t.s=="BI" && t.beg==pl->inlineoff) {
            in = pdfinlineat(&p, &x);
        }
        if (!in.ok) {
            r.err = p.why.data ? p.why : Str("its content stream does not hold it");
            return r;
        }
        PdfObj cs = pdfget(&p, in.dict, "CS");
        cs = cs.kind ? cs : pdfget(&p, in.dict, "ColorSpace");
        if (cs.kind == PDFO_NAME) {
            res = pdfget(&p, dict, "Resources");  // a form's
            res = res.kind==PDFO_DICT ? res : pdfpageres(&p, pl->page);
        }
        dict = in.dict;
        raw  = in.data;
    }

    PdfSamples sm = {};
    PdfSamples sp = {};
    Str        why = pdfsamplesof(&p, dict, res, pl->image, &sp);
    if (why.data) {
        r.err = p.why.data ? p.why : why;  // a lookup stopped for memory
        return r;
    }
    sp.fill[0] = pl->fill[0];
    sp.fill[1] = pl->fill[1];
    sp.fill[2] = pl->fill[2];

    // The decoded budget, sized for the whole document's text, grows by
    // the samples the image says it holds, so that a big image in a
    // small file still decodes; codecs' input is taken as it comes
    iz      cap = pl->image==PDFI_NONE && sp.cs>=0 ? pdfsamplebytes(&sp) : 0;
    p.decoded += cap + cap/2;  // pdfinflate's growing guesses cost 4/3 at most
    PdfData d   = pdfdecode(&p, &p.page, dict, raw, cap);
    if (!d.data.data || d.image==PDFI_JPX || d.image==PDFI_JBIG2) {
        r.err = p.why.data ? p.why : d.image ? Str("its kind is not shown")
                                             : Str("its data cannot be decoded");
        return r;
    }
    if (pl->inlineoff<0 && pdfsoftmask(&p, dict, &sm)) {
        sp.smask = &sm;
    }
    if (p.why.data) {
        r.err = p.why;  // out of memory, or budget: not the image as it is
        return r;
    }

    Arena mem  = p.page;
    i64   work = p.work;
    sp.data = d.data;
    switch (d.image) {
    case PDFI_NONE:
        if (sp.cs < 0) {
            r.err = "an unsupported colour space";
            return r;
        }
        r = pdfshowsamples(perm, mem, &sp, edge, pixels, maxb64, &work);
        break;
    case PDFI_DCT:
        if (sp.cs < 0) {
            i32 n = pdfjpeg(d.data).ncomp;
            sp.cs = n==1 ? PDFCS_GRAY : n==4 ? PDFCS_CMYK : PDFCS_RGB;
        }
        r = pdfshowjpeg(perm, mem, &sp, edge, pixels, maxb64);
        break;
    case PDFI_CCITT: {
        PdfObj      fp = d.parms;
        CcittParams cp = {};
        PdfObj      ba = pdfget(&p, fp, "EncodedByteAlign");
        PdfObj      bi = pdfget(&p, fp, "BlackIs1");
        cp.k        = (i32)pdfclamp(pdfint(&p, fp, "K", 0), -1, 1);
        cp.columns  = (i32)pdfclamp(pdfint(&p, fp, "Columns", sp.w), -1, PDFIMG_EDGE+1);
        cp.rows     = (i32)pdfclamp(pdfint(&p, fp, "Rows", sp.h), -1, PDFIMG_EDGE+1);
        cp.align    = ba.kind==PDFO_BOOL && ba.b;
        cp.blackis1 = bi.kind==PDFO_BOOL && bi.b;
        Ccitt c = pdfccitt(&mem, d.data, cp, (i64)PDF_DECODEPIXELS*8, &work);
        if (!c.bits.data) {
            r.err = c.err;
            return r;
        }
        sp.data = c.bits;
        sp.w    = c.cols;
        sp.h    = c.rows;
        sp.bpc  = 1;
        b32 one = sp.mask || sp.cs==PDFCS_GRAY || sp.cs==PDFCS_INDEXED ||
                  (sp.cs==PDFCS_TINT && sp.ncomp==1);
        sp.cs   = one ? sp.cs : PDFCS_GRAY;
        r = pdfshowsamples(perm, mem, &sp, edge, pixels, maxb64, &work);
    } break;
    }
    if (!r.data.data) {
        *perm = save;
        r.err = r.err.data ? r.err : Str("it cannot be shown");
    }
    return r;
}

// The images a window of lines [first, last] shows, as indices into
// places in order of their lines, and each one's limits: those given,
// within PDF_SHOWPIXELS and PDF_SHOWBYTES shared among them. Each is
// shown once by its object (an inline image is its own), and a stencil
// once per colour; those under PDF_PICKMIN on a side, and those that
// cannot be decoded (JPEG 2000, JBIG2), not at all. Past PDF_IMAGES the
// window is cut before the next one's line, so that the next window
// begins with it. Its scratch is for sorting; when that does not fit,
// none is picked.
struct PdfPick {
    i32 at[PDF_IMAGES];
    i32 n;
    iz  last;    // the window's last line, perhaps cut
    b32 cut;     // so cut
    i64 edge;    // each image's limits
    i64 pixels;
    iz  maxb64;
};

struct PdfPickAt {
    iz  line;
    i32 at;
};

static b32 pdfpickless(PdfPickAt *a, PdfPickAt *b)
{
    return a->line!=b->line ? a->line<b->line : a->at<b->at;
}

static PdfPick pdfpick(Arena scratch, Slice<PdfPlace> places, iz first, iz last,
                       i64 edge, i64 pixels, iz maxb64)
{
    PdfPick r = {};
    r.last = last;
    iz n = 0;
    for (iz i = 0; i < places.len; i++) {
        PdfPlace *pl = places.data + i;
        n += pl->line>=first && pl->line<=last;
    }
    iz pad = -(uz)scratch.beg & (alignof(PdfPickAt) - 1);
    if (!n || !pdffits(&scratch, pad, 2*n, sizeof(PdfPickAt))) {
        r.edge   = edge;
        r.pixels = pixels<PDF_SHOWPIXELS ? pixels : (i64)PDF_SHOWPIXELS;
        r.maxb64 = maxb64<PDF_SHOWBYTES ? maxb64 : (iz)PDF_SHOWBYTES;
        return r;
    }
    PdfPickAt *c = alloc<PdfPickAt>(&scratch, n);
    iz         k = 0;
    for (iz i = 0; i < places.len; i++) {
        PdfPlace *pl = places.data + i;
        if (pl->line>=first && pl->line<=last) {
            c[k].line = pl->line;
            c[k].at   = (i32)i;
            k++;
        }
    }
    sort(c, n, pdfpickless, scratch);

    for (iz i = 0; i < n; i++) {
        PdfPlace *pl = places.data + c[i].at;
        if (pl->w<PDF_PICKMIN || pl->h<PDF_PICKMIN ||
            pl->image==PDFI_JPX || pl->image==PDFI_JBIG2) {
            continue;
        }
        b32 seen = 0;
        for (i32 j = 0; j<r.n && !seen; j++) {
            PdfPlace *q = places.data + r.at[j];
            seen = q->id==pl->id && (!pl->mask ||
                   (q->fill[0]==pl->fill[0] && q->fill[1]==pl->fill[1] &&
                    q->fill[2]==pl->fill[2]));
        }
        if (seen) {
            continue;
        } else if (r.n == PDF_IMAGES) {
            // On the line of the last picked (stencils of one image in
            // more colours), a cut would drop that one too: not shown,
            // and the cut goes before the next line holding an image
            if (pl->line > places.data[r.at[r.n-1]].line) {
                r.last = pl->line - 1;
                r.cut  = 1;
                break;
            }
            continue;
        }
        r.at[r.n++] = c[i].at;
    }

    i64 share = r.n ? PDF_SHOWPIXELS/r.n : PDF_SHOWPIXELS;
    iz  bytes = r.n ? PDF_SHOWBYTES/r.n : PDF_SHOWBYTES;
    r.edge    = edge;
    r.pixels  = pixels<share ? pixels : share;
    r.maxb64  = maxb64<bytes ? maxb64 : bytes;
    return r;
}
