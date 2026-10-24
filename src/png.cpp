// PNG encoding, for the desktop helper's screenshots and for images
// converted or shrunk to attach, and a box filter that shrinks an image.
// Decoding, of PNG and the rest, is image.cpp's, through Wuffs.
//
// This is free and unencumbered software released into the public domain.

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


// PNG encoding: Sub/Up row filters, LZ77 deflate with fixed Huffman
// codes or stored blocks, whichever is smaller (winctl's encoder), for
// the desktop helper's screenshots and images shrunk to attach.

static u32 zcrc32(u32 crc, u8 *p, iz len)
{
    static u32 table[256];
    if (!table[1]) {
        for (u32 i = 0; i < 256; i++) {
            u32 c = i;
            for (i32 k = 0; k < 8; k++) {
                c = c&1 ? 0xedb88320 ^ (c>>1) : c>>1;
            }
            table[i] = c;
        }
    }
    crc = ~crc;
    for (iz i = 0; i < len; i++) {
        crc = table[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
    }
    return ~crc;
}

static u32 zadler32(u8 *p, iz len)
{
    u32 a = 1, b = 0;
    while (len > 0) {
        iz n = len<5552 ? len : 5552;  // no overflow before the modulo
        for (iz i = 0; i < n; i++) {
            a += p[i];
            b += a;
        }
        a %= 65521;
        b %= 65521;
        p   += n;
        len -= n;
    }
    return b<<16 | a;
}

struct ZBits {
    u8 *buf;
    iz  len;
    iz  cap;
    u64 acc;
    i32 nbits;
};

static void zputbits(ZBits *b, u32 v, i32 n)
{
    b->acc   |= (u64)v << b->nbits;
    b->nbits += n;
    while (b->nbits >= 8) {
        assert(b->len < b->cap);
        b->buf[b->len++] = (u8)b->acc;
        b->acc  >>= 8;
        b->nbits -= 8;
    }
}

// Huffman codes are packed most significant bit first.
static void zputcode(ZBits *b, u32 code, i32 n)
{
    u32 r = 0;
    for (i32 i = 0; i < n; i++) {
        r = r<<1 | (code>>i & 1);
    }
    zputbits(b, r, n);
}

static void zputsym(ZBits *b, i32 sym)
{
    if (sym < 144) {
        zputcode(b, (u32)(0x30 + sym), 8);
    } else if (sym < 256) {
        zputcode(b, (u32)(0x190 + sym - 144), 9);
    } else if (sym < 280) {
        zputcode(b, (u32)(sym - 256), 7);
    } else {
        zputcode(b, (u32)(0xc0 + sym - 280), 8);
    }
}

static void zputmatch(ZBits *b, i32 len, i32 dist)
{
    static i32 const lenbase[] = {
        3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
        35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
    };
    static i32 const lenextra[] = {
        0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
        3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
    };
    static i32 const distbase[] = {
        1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257,
        385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289,
        16385, 24577
    };
    static i32 const distextra[] = {
        0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7,
        8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
    };
    i32 l = countof(lenbase) - 1;
    for (; lenbase[l] > len; l--) {}
    zputsym(b, 257 + l);
    zputbits(b, (u32)(len - lenbase[l]), lenextra[l]);
    i32 d = countof(distbase) - 1;
    for (; distbase[d] > dist; d--) {}
    zputcode(b, (u32)d, 5);
    zputbits(b, (u32)(dist - distbase[d]), distextra[d]);
}

// Deflate into a single fixed-Huffman block. The output must hold at
// least len*9/8 + 16 bytes, the worst case of all literals.
static iz zdeflate(Arena scratch, u8 *out, iz cap, u8 *p, iz len)
{
    enum {
        WINBITS  = 15,
        WINSIZE  = 1 << WINBITS,
        HASHBITS = 15,
        MAXCHAIN = 16,
        MINMATCH = 3,
        MAXMATCH = 258,
    };
    i32 *head = alloc<i32>(&scratch, 1<<HASHBITS);  // position+1, 0 empty
    i32 *prev = alloc<i32>(&scratch, WINSIZE);

    ZBits b = {};
    b.buf = out;
    b.cap = cap;
    zputbits(&b, 1, 1);  // final block
    zputbits(&b, 1, 2);  // fixed Huffman codes

    for (iz i = 0; i < len;) {
        i32 best = 0;
        i32 dist = 0;
        if (len-i >= MINMATCH) {
            u32 key = (u32)p[i]<<16 | (u32)p[i+1]<<8 | p[i+2];
            u32 h   = key*0x9e3779b1u >> (32 - HASHBITS);
            iz  max = len-i<MAXMATCH ? len-i : (iz)MAXMATCH;
            iz  at  = head[h] - 1;
            for (i32 n = 0; at>=0 && i-at<=WINSIZE && n<MAXCHAIN; n++) {
                iz k = 0;
                for (; k<max && p[at+k]==p[i+k]; k++) {}
                if (k > best) {
                    best = (i32)k;
                    dist = (i32)(i - at);
                    if (k == max) {
                        break;
                    }
                }
                iz next = prev[at & (WINSIZE-1)] - 1;
                if (next >= at) {
                    break;  // an overwritten slot
                }
                at = next;
            }
            prev[i & (WINSIZE-1)] = head[h];
            head[h] = (i32)(i + 1);
        }

        if (best >= MINMATCH) {
            zputmatch(&b, best, dist);
            for (iz j = i+1; j<i+best && len-j>=MINMATCH; j++) {
                u32 key = (u32)p[j]<<16 | (u32)p[j+1]<<8 | p[j+2];
                u32 hj  = key*0x9e3779b1u >> (32 - HASHBITS);
                prev[j & (WINSIZE-1)] = head[hj];
                head[hj] = (i32)(j + 1);
            }
            i += best;
        } else {
            zputsym(&b, p[i]);
            i++;
        }
    }
    zputsym(&b, 256);
    if (b.nbits) {
        zputbits(&b, 0, 8 - b.nbits);
    }
    return b.len;
}

static iz zstoredsize(iz len)
{
    iz blocks = len ? (len + 65534)/65535 : 1;
    return len + 5*blocks;
}

static iz zstored(u8 *out, u8 *p, iz len)
{
    iz n = 0;
    iz i = 0;
    do {
        iz  chunk = len-i<65535 ? len-i : 65535;
        u32 c     = (u32)chunk;
        out[n++]  = i+chunk == len;  // final block flag, BTYPE 00
        out[n++]  = (u8)(c >> 0);
        out[n++]  = (u8)(c >> 8);
        out[n++]  = (u8)(~c >> 0);
        out[n++]  = (u8)(~c >> 8);
        copybytes(out+n, p+i, chunk);
        n += chunk;
        i += chunk;
    } while (i < len);
    return n;
}

static void zput32be(u8 *p, u32 v)
{
    p[0] = (u8)(v >> 24);
    p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >>  8);
    p[3] = (u8)(v >>  0);
}

static i64 zfiltercost(u8 *row, iz len)
{
    i64 sum = 0;
    for (iz i = 0; i < len; i++) {
        i32 v = (i32)(signed char)row[i];
        sum += v<0 ? -v : v;
    }
    return sum;
}

// Encode top-down pixels as a PNG: 32-bit BGRX (bgrx) as 24-bit RGB,
// else RGBA as RGBA, or as RGB where every pixel is opaque.
static Str pngencodez(Arena *a, u8 *px, i32 w, i32 h, b32 bgrx)
{
    b32 opaque = 1;
    for (iz i = 0; !bgrx && opaque && i<(iz)w*h; i++) {
        opaque = px[i*4+3] == 255;
    }
    i32 ch     = opaque ? 3 : 4;  // channels written
    iz  stride = (iz)w * ch;
    iz  rawlen = (stride + 1) * h;
    u8 *raw    = allocbytes(a, rawlen);
    u8 *prior  = alloc<u8>(a, stride);  // the row above, zero for the first
    u8 *cur    = allocbytes(a, stride);
    u8 *sub    = allocbytes(a, stride);
    u8 *up     = allocbytes(a, stride);

    for (i32 y = 0; y < h; y++) {
        u8 *src = px + (iz)y*w*4;
        for (iz x = 0; x < w; x++) {
            u8 *p = src + x*4;
            u8 *q = cur + x*ch;
            q[0] = bgrx ? p[2] : p[0];
            q[1] = p[1];
            q[2] = bgrx ? p[0] : p[2];
            if (ch == 4) {
                q[3] = p[3];
            }
        }
        for (iz i = 0; i < stride; i++) {
            sub[i] = (u8)(cur[i] - (i>=ch ? cur[i-ch] : 0));
            up[i]  = (u8)(cur[i] - prior[i]);
        }
        i64 cnone = zfiltercost(cur, stride);
        i64 csub  = zfiltercost(sub, stride);
        i64 cup   = zfiltercost(up,  stride);
        u8 *dst   = raw + y*(stride + 1);
        if (csub<=cup && csub<cnone) {
            dst[0] = 1;
            copybytes(dst+1, sub, stride);
        } else if (cup < cnone) {
            dst[0] = 2;
            copybytes(dst+1, up, stride);
        } else {
            dst[0] = 0;
            copybytes(dst+1, cur, stride);
        }
        u8 *t = prior;
        prior = cur;
        cur   = t;
    }

    static u8 const sig[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    iz  cap = 8 + 25 + 8 + 2 + rawlen + rawlen/8 + 64 + 4 + 4 + 12;
    u8 *png = allocbytes(a, cap);
    iz  n   = 0;

    copybytes(png, (u8 *)sig, 8);
    n += 8;
    zput32be(png+n, 13);
    copybytes(png+n+4, (u8 *)"IHDR", 4);
    zput32be(png+n+8,  (u32)w);
    zput32be(png+n+12, (u32)h);
    png[n+16] = 8;  // bit depth
    png[n+17] = ch==4 ? 6 : 2;  // truecolor, with alpha or not
    png[n+18] = 0;  // deflate
    png[n+19] = 0;  // adaptive filtering
    png[n+20] = 0;  // no interlace
    zput32be(png+n+21, zcrc32(0, png+n+4, 17));
    n += 25;

    iz idat = n;
    copybytes(png+n+4, (u8 *)"IDAT", 4);
    n += 8;
    png[n++] = 0x78;  // zlib: deflate, 32K window
    png[n++] = 0x01;
    iz z = zdeflate(*a, png+n, cap-n, raw, rawlen);
    if (z >= zstoredsize(rawlen)) {
        z = zstored(png+n, raw, rawlen);
    }
    n += z;
    zput32be(png+n, zadler32(raw, rawlen));
    n += 4;
    zput32be(png+idat, (u32)(n - idat - 8));
    zput32be(png+n, zcrc32(0, png+idat+4, n-idat-4));
    n += 4;

    zput32be(png+n, 0);
    copybytes(png+n+4, (u8 *)"IEND", 4);
    zput32be(png+n+8, zcrc32(0, png+n+4, 4));
    n += 12;
    return {png, n};
}
