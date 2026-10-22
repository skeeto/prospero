// PNG decoding, for the transcript's screenshots: inflate, the row
// filters and the pixel formats, into RGBA, and a box filter that
// shrinks an image for a thumbnail
//
// Every PNG of the standard decodes but an interlaced one, which no
// screenshot is: any bit depth and color type, with a palette's
// transparency (tRNS), and other transparency ignored. Checksums go
// unchecked. The input is untrusted, from a session's directory, maybe
// on another machine: a bad PNG fails with a reason, never reads or
// writes out of bounds, and its pixels are bounded by the caller.
//
// Huffman codes decode INF_FAST bits at a time through a table, and
// longer ones a bit at a time over the canonical code (as zlib's puff
// does), which in practice is rare.
//
// This is free and unencumbered software released into the public domain.

enum : i32 {
    INF_FAST = 10,  // bits a Huffman table looks up at once
    INF_MAXBITS = 15,
};

struct Huff {
    i32 count[INF_MAXBITS+1];  // codes of each length
    i32 symbol[288];           // in canonical order
    i32 fast[1<<INF_FAST];     // length<<9 | symbol, 0 if longer
};

struct Inflate {
    Str  in;
    iz   pos;
    u64  bits;
    i32  nbits;
    i32  pad;    // zero bytes taken past the end of in
    u8  *out;
    iz   len;
    iz   cap;
    Str  err;
};

// Fill the bit buffer to at least 57 bits, with zeros past the input's
// end, which fail the stream once more were taken than it could hold.
static void infill(Inflate *z)
{
    while (z->nbits <= 56) {
        u64 c = 0;
        if (z->pos < z->in.len) {
            c = z->in[z->pos++];
        } else if (++z->pad > 8 && !z->err.data) {
            z->err = "the compressed data ends early";
        }
        z->bits  |= c << z->nbits;
        z->nbits += 8;
    }
}

static i32 inbits(Inflate *z, i32 n)
{
    if (z->nbits < n) {
        infill(z);
    }
    i32 r = (i32)(z->bits & (((u64)1<<n) - 1));
    z->bits  >>= n;
    z->nbits  -= n;
    return r;
}

// A table from code lengths, 0 for unused symbols. Fails if the lengths
// are over-subscribed. An incomplete code is allowed: an unused code
// then fails the stream where it appears.
static b32 huffbuild(Huff *h, u8 *lens, i32 n)
{
    __builtin_memset(h, 0, sizeof(*h));
    for (i32 i = 0; i < n; i++) {
        h->count[lens[i]]++;
    }
    h->count[0] = 0;
    i32 left = 1;
    for (i32 len = 1; len <= INF_MAXBITS; len++) {
        left = 2*left - h->count[len];
        if (left < 0) {
            return 0;
        }
    }

    i32 offs[INF_MAXBITS+1] = {};
    i32 next[INF_MAXBITS+1] = {};
    for (i32 len = 1, code = 0; len <= INF_MAXBITS; len++) {
        offs[len] = offs[len-1] + h->count[len-1];
        code      = (code + h->count[len-1]) << 1;
        next[len] = code;
    }
    for (i32 sym = 0; sym < n; sym++) {
        i32 len = lens[sym];
        if (!len) {
            continue;
        }
        h->symbol[offs[len]++] = sym;
        i32 code = next[len]++;
        if (len <= INF_FAST) {
            // Codes go most significant bit first: reversed for lookup
            i32 rev = 0;
            for (i32 b = 0; b < len; b++) {
                rev |= (code>>b & 1) << (len-1-b);
            }
            for (i32 k = rev; k < 1<<INF_FAST; k += 1<<len) {
                h->fast[k] = len<<9 | sym;
            }
        }
    }
    return 1;
}

static i32 huffdecode(Inflate *z, Huff *h)
{
    if (z->nbits < INF_MAXBITS) {
        infill(z);
    }
    i32 e = h->fast[z->bits & ((1<<INF_FAST) - 1)];
    if (e) {
        z->bits  >>= e >> 9;
        z->nbits  -= e >> 9;
        return e & 511;
    }
    i32 code = 0, first = 0, index = 0;
    for (i32 len = 1; len <= INF_MAXBITS; len++) {
        code |= inbits(z, 1);
        i32 count = h->count[len];
        if (code-first < count) {
            return h->symbol[index + (code - first)];
        }
        index += count;
        first  = (first + count) << 1;
        code <<= 1;
    }
    z->err = z->err.data ? z->err : Str("a code the table lacks");
    return -1;
}

static b32 infcodes(Inflate *z, Huff *lit, Huff *dist)
{
    static i32 const lbase[] = {
        3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
        35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258,
    };
    static u8 const lext[] = {
        0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
        3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0,
    };
    static i32 const dbase[] = {
        1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
        257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
        8193, 12289, 16385, 24577,
    };
    static u8 const dext[] = {
        0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
        7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13,
    };
    for (;;) {
        i32 sym = huffdecode(z, lit);
        if (z->err.data) {
            return 0;
        } else if (sym < 256) {
            if (z->len == z->cap) {
                z->err = "more image data than its size";
                return 0;
            }
            z->out[z->len++] = (u8)sym;
        } else if (sym == 256) {
            return 1;
        } else if (sym > 285) {
            z->err = "a bad length code";
            return 0;
        } else {
            sym -= 257;
            iz  len = lbase[sym] + inbits(z, lext[sym]);
            i32 d   = huffdecode(z, dist);
            if (z->err.data) {
                return 0;
            } else if (d > 29) {
                z->err = "a bad distance code";
                return 0;
            }
            iz back = dbase[d] + inbits(z, dext[d]);
            if (back > z->len) {
                z->err = "a distance before the start";
                return 0;
            } else if (len > z->cap-z->len) {
                z->err = "more image data than its size";
                return 0;
            }
            u8 *p = z->out + z->len;
            for (iz i = 0; i < len; i++) {
                p[i] = p[i-back];  // overlaps when back < len
            }
            z->len += len;
        }
    }
}

static b32 infdynamic(Inflate *z, Huff *lit, Huff *dist)
{
    static u8 const order[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15,
    };
    i32 nlit  = inbits(z, 5) + 257;
    i32 ndist = inbits(z, 5) + 1;
    i32 ncode = inbits(z, 4) + 4;
    if (nlit>286 || ndist>30) {
        z->err = "too many codes";
        return 0;
    }
    u8 lens[286+30] = {};
    for (i32 i = 0; i < ncode; i++) {
        lens[order[i]] = (u8)inbits(z, 3);
    }
    Huff cl;
    if (!huffbuild(&cl, lens, 19)) {
        z->err = "bad code lengths";
        return 0;
    }
    __builtin_memset(lens, 0, 19);
    for (i32 i = 0; i < nlit+ndist;) {
        i32 sym = huffdecode(z, &cl);
        if (z->err.data) {
            return 0;
        } else if (sym < 16) {
            lens[i++] = (u8)sym;
            continue;
        }
        i32 rep = 0, len = 0;
        if (sym == 16) {
            if (!i) {
                z->err = "a repeat with nothing before it";
                return 0;
            }
            len = lens[i-1];
            rep = 3 + inbits(z, 2);
        } else if (sym == 17) {
            rep = 3 + inbits(z, 3);
        } else {
            rep = 11 + inbits(z, 7);
        }
        if (rep > nlit+ndist-i) {
            z->err = "too many code lengths";
            return 0;
        }
        for (; rep; rep--) {
            lens[i++] = (u8)len;
        }
    }
    if (!lens[256]) {
        z->err = "no end-of-block code";
        return 0;
    }
    if (!huffbuild(lit, lens, nlit) || !huffbuild(dist, lens+nlit, ndist)) {
        z->err = "bad code lengths";
        return 0;
    }
    return 1;
}

// Inflate a zlib stream into exactly cap bytes at out. Returns the
// error, null if none.
static Str zinflate(Arena scratch, Str in, u8 *out, iz cap)
{
    Inflate z = {};
    z.in  = in;
    z.out = out;
    z.cap = cap;
    if (in.len<2 || (in[0]&15)!=8 || (in[0]>>4)>7 ||
            ((i32)in[0]<<8 | in[1])%31 || in[1]&0x20) {
        return "not a zlib stream";
    }
    z.pos = 2;

    Huff *lit  = alloc<Huff>(&scratch);
    Huff *dist = alloc<Huff>(&scratch);
    Huff *flit = 0;
    Huff *fdst = 0;
    for (b32 last = 0; !last;) {
        last = inbits(&z, 1);
        switch (inbits(&z, 2)) {
        case 0: {
            inbits(&z, z.nbits & 7);  // to a byte boundary
            i32 len  = inbits(&z, 16);
            i32 nlen = inbits(&z, 16);
            if (len != (~nlen & 0xffff)) {
                return "a stored block's length is corrupt";
            } else if (len > z.cap-z.len) {
                return "more image data than its size";
            }
            for (i32 i = 0; i < len; i++) {
                z.out[z.len++] = (u8)inbits(&z, 8);
            }
        } break;
        case 1:
            if (!flit) {
                u8 lens[288+30];
                for (i32 i = 0; i < 288; i++) {
                    lens[i] = i<144 ? 8 : i<256 ? 9 : i<280 ? 7 : 8;
                }
                for (i32 i = 0; i < 30; i++) {
                    lens[288+i] = 5;
                }
                flit = alloc<Huff>(&scratch);
                fdst = alloc<Huff>(&scratch);
                huffbuild(flit, lens, 288);
                huffbuild(fdst, lens+288, 30);
            }
            if (!infcodes(&z, flit, fdst)) {
                return z.err;
            }
            break;
        case 2:
            if (!infdynamic(&z, lit, dist) || !infcodes(&z, lit, dist)) {
                return z.err;
            }
            break;
        case 3:
            return "a bad block type";
        }
        if (z.err.data) {
            return z.err;
        }
    }
    return z.len==z.cap ? Str{} : Str("less image data than its size");
}


// PNG

struct Png {
    u8  *rgba;    // width*height pixels, top row first, null on error
    i32  width;
    i32  height;
    Str  err;     // why it failed, null if it did not
};

static u32 pngu32(u8 *p)
{
    return (u32)p[0]<<24 | (u32)p[1]<<16 | (u32)p[2]<<8 | p[3];
}

static u8 paeth(i32 a, i32 b, i32 c)
{
    i32 p  = a + b - c;
    i32 pa = p>a ? p-a : a-p;
    i32 pb = p>b ? p-b : b-p;
    i32 pc = p>c ? p-c : c-p;
    return (u8)(pa<=pb && pa<=pc ? a : pb<=pc ? b : c);
}

// Decode a PNG of at most maxpixels pixels into RGBA. The pixels go in
// a, and the temporaries after them, which are left for the caller to
// discard with the arena: as much again as the image, roughly.
static Png pngdecode(Arena *a, Str png, i64 maxpixels)
{
    Png r = {};
    if (png.len<8 || !(takehead(png, 8) == Str("\x89PNG\r\n\x1a\n"))) {
        r.err = "not a PNG";
        return r;
    }

    // Chunks: the header, the palette, its transparency, and the size of
    // the image data, gathered into one stream in a second pass
    i64 w = 0, h = 0;
    i32 depth = 0, color = 0;
    u8  pal[256*4];
    i32 npal  = 0;
    iz  zlen  = 0;
    b32 ended = 0;
    for (iz off = 8; !ended;) {
        if (png.len-off < 12) {
            r.err = "it ends early";
            return r;
        }
        u32 len  = pngu32(png.data+off);
        Str type = {png.data+off+4, 4};
        if (len > (u32)(png.len-off-12)) {
            r.err = "it ends early";
            return r;
        }
        u8 *d = png.data + off + 8;
        if (off == 8) {
            if (!(type == Str("IHDR")) || len!=13) {
                r.err = "no header";
                return r;
            }
            w     = pngu32(d);
            h     = pngu32(d+4);
            depth = d[8];
            color = d[9];
            if (d[10] || d[11]) {
                r.err = "an unknown compression or filter method";
                return r;
            } else if (d[12]) {
                r.err = "interlaced";
                return r;
            }
            b32 ok = 0;
            switch (color) {
            case 0: ok = depth==1 || depth==2 || depth==4 || depth==8 || depth==16; break;
            case 3: ok = depth==1 || depth==2 || depth==4 || depth==8; break;
            case 2:
            case 4:
            case 6: ok = depth==8 || depth==16; break;
            }
            if (!ok) {
                r.err = "an unknown pixel format";
                return r;
            } else if (w<1 || h<1 || w>0x7fffffff || h>0x7fffffff) {
                r.err = "a bad size";
                return r;
            } else if (w > maxpixels/h) {
                r.err = "too big to show";
                return r;
            }
        } else if (type == Str("PLTE")) {
            if (len%3 || len>3*256) {
                r.err = "a bad palette";
                return r;
            }
            npal = (i32)len / 3;
            for (i32 i = 0; i < npal; i++) {
                pal[4*i+0] = d[3*i+0];
                pal[4*i+1] = d[3*i+1];
                pal[4*i+2] = d[3*i+2];
                pal[4*i+3] = 255;
            }
        } else if (type == Str("tRNS")) {
            for (i32 i = 0; color==3 && i<(i32)len && i<npal; i++) {
                pal[4*i+3] = d[i];
            }
        } else if (type == Str("IDAT")) {
            zlen += len;
        } else if (type == Str("IEND")) {
            ended = 1;
        }
        off += 12 + (iz)len;
    }
    if (color==3 && !npal) {
        r.err = "no palette";
        return r;
    }
    for (i32 i = npal; i < 256; i++) {
        pal[4*i+0] = pal[4*i+1] = pal[4*i+2] = 0;
        pal[4*i+3] = 255;
    }

    i32 chans   = color==2 ? 3 : color==4 ? 2 : color==6 ? 4 : 1;
    i32 bits    = chans * depth;
    iz  rowlen  = (iz)((w*bits + 7) / 8);
    i32 bpp     = bits<8 ? 1 : bits/8;  // bytes a filter looks back
    u8 *rgba    = allocbytes(a, (iz)(w*h*4));
    Arena scratch = *a;  // the rest, after the pixels

    u8 *zdata = allocbytes(&scratch, zlen);
    iz  zn    = 0;
    for (iz off = 8; zn < zlen;) {
        u32 len = pngu32(png.data+off);
        if (Str{png.data+off+4, 4} == Str("IDAT")) {
            copybytes(zdata+zn, png.data+off+8, len);
            zn += len;
        }
        off += 12 + (iz)len;
    }
    iz  rawlen = (iz)h * (1 + rowlen);
    u8 *raw    = allocbytes(&scratch, rawlen);
    Str err    = zinflate(scratch, {zdata, zlen}, raw, rawlen);
    if (err.data) {
        r.err = err;
        return r;
    }

    // Filters, row by row in place, against a row of zeros above the first
    u8 *prev = alloc<u8>(&scratch, rowlen);
    for (i64 y = 0; y < h; y++) {
        u8 *row = raw + y*(1 + rowlen);
        u8 *p   = row + 1;
        switch (row[0]) {
        case 0: break;
        case 1:
            for (iz i = bpp; i < rowlen; i++) p[i] = (u8)(p[i] + p[i-bpp]);
            break;
        case 2:
            for (iz i = 0; i < rowlen; i++) p[i] = (u8)(p[i] + prev[i]);
            break;
        case 3:
            for (iz i = 0; i < rowlen; i++) {
                i32 left = i>=bpp ? p[i-bpp] : 0;
                p[i] = (u8)(p[i] + (left + prev[i])/2);
            }
            break;
        case 4:
            for (iz i = 0; i < rowlen; i++) {
                i32 left = i>=bpp ? p[i-bpp]    : 0;
                i32 ul   = i>=bpp ? prev[i-bpp] : 0;
                p[i] = (u8)(p[i] + paeth(left, prev[i], ul));
            }
            break;
        default:
            r.err = "a bad row filter";
            return r;
        }
        prev = p;
    }

    // Pixels, from samples of any depth: 16 bits keep their high byte,
    // and fewer than 8 scale up to the full range
    for (i64 y = 0; y < h; y++) {
        u8 *p   = raw + y*(1 + rowlen) + 1;
        u8 *out = rgba + y*w*4;
        for (i64 x = 0; x < w; x++, out += 4) {
            if (depth < 8) {
                i64 bit = x * depth;
                i32 v   = p[bit>>3] >> (8 - depth - (bit&7)) & ((1<<depth) - 1);
                if (color == 3) {
                    copybytes(out, pal+4*v, 4);
                } else {
                    u8 g = (u8)(v * (255 / ((1<<depth) - 1)));
                    out[0] = out[1] = out[2] = g;
                    out[3] = 255;
                }
                continue;
            }
            i32 step = depth / 8;
            u8 *s    = p + x*chans*step;
            switch (color) {
            case 0: out[0] = out[1] = out[2] = s[0]; out[3] = 255; break;
            case 2: out[0] = s[0]; out[1] = s[step]; out[2] = s[2*step]; out[3] = 255; break;
            case 3: copybytes(out, pal+4*s[0], 4); break;
            case 4: out[0] = out[1] = out[2] = s[0]; out[3] = s[step]; break;
            case 6: out[0] = s[0]; out[1] = s[step]; out[2] = s[2*step]; out[3] = s[3*step]; break;
            }
        }
    }
    r.rgba   = rgba;
    r.width  = (i32)w;
    r.height = (i32)h;
    return r;
}

// Shrink an image to tw by th by averaging the box of pixels behind
// each, weighted by their alpha. Each pixel lands in exactly one box.
static u8 *shrink(Arena *a, u8 *rgba, i32 w, i32 h, i32 tw, i32 th)
{
    assert(tw>0 && th>0 && tw<=w && th<=h);
    u8  *r    = allocbytes(a, (iz)tw*th*4);
    u64 *sums = alloc<u64>(a, (iz)tw*4);  // left with the arena
    for (i32 ty = 0; ty < th; ty++) {
        i32 y0 = (i32)((i64)ty*h/th);
        i32 y1 = (i32)((i64)(ty+1)*h/th);
        __builtin_memset(sums, 0, (uz)tw*4*sizeof(u64));
        for (i32 y = y0; y < y1; y++) {
            u8 *row = rgba + (iz)y*w*4;
            for (i32 tx = 0; tx < tw; tx++) {
                i32  x0 = (i32)((i64)tx*w/tw);
                i32  x1 = (i32)((i64)(tx+1)*w/tw);
                u64 *s  = sums + tx*4;
                for (i32 x = x0; x < x1; x++) {
                    u8 *p = row + (iz)x*4;
                    s[0] += (u64)p[0] * p[3];
                    s[1] += (u64)p[1] * p[3];
                    s[2] += (u64)p[2] * p[3];
                    s[3] += p[3];
                }
            }
        }
        for (i32 tx = 0; tx < tw; tx++) {
            u64 *s   = sums + tx*4;
            u8  *out = r + ((iz)ty*tw + tx)*4;
            u64  n   = (u64)(y1-y0) * (u64)((i64)(tx+1)*w/tw - (i64)tx*w/tw);
            u64  al  = s[3];
            out[0] = (u8)(al ? (s[0] + al/2)/al : 0);
            out[1] = (u8)(al ? (s[1] + al/2)/al : 0);
            out[2] = (u8)(al ? (s[2] + al/2)/al : 0);
            out[3] = (u8)((al + n/2)/n);
        }
    }
    return r;
}
