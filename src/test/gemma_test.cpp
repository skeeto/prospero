// Built-in model tests (gemma.cpp): half floats and rounding, the
// kernels this processor runs against the plain ones, the weight
// layouts, the tokenizer on a made-up vocabulary, and the GGUF reader on
// broken files. The model itself is too big for unit tests; the
// developers' builtin-logits command checks it against gemma4.c.

static u64 gmtrand(u64 *s)
{
    *s = *s*0x3243f6a8885a308d + 1;
    u64 x = *s;
    x ^= x >> 29;
    x *= 0xbf58476d1ce4e5b9;
    return x ^ x>>32;
}

static f32 gmtunit(u64 *s)  // -1 to 1
{
    return (f32)(i32)(gmtrand(s)>>40 & 0xffffff) / (f32)(1<<23) - 1;
}

// Whether a and b, of n, agree to a relative tolerance of their scale.
static b32 gmtclose(f32 *a, f32 *b, iz n, f32 tol)
{
    f32 scale = 0;
    for (iz i = 0; i < n; i++) {
        scale = gmabs(a[i])>scale ? gmabs(a[i]) : scale;
    }
    for (iz i = 0; i < n; i++) {
        if (gmabs(a[i]-b[i]) > tol*scale) {
            return 0;
        }
    }
    return 1;
}

static void test_gemmanumbers(Test *t)
{
    Arena a = t->perm;

    expect(t, "gemma.half.one", istr(&a, (i64)gmhalf(0x3c00)), "1");
    expect(t, "gemma.half.neg", istr(&a, (i64)gmhalf(0xc000)), "-2");
    expect(t, "gemma.half.max", istr(&a, (i64)gmhalf(0x7bff)), "65504");
    expect(t, "gemma.half.tiny", istr(&a, gmhalf(0x0001)==1.0f/(1<<24)), "1");
    expect(t, "gemma.half.sub", istr(&a, gmhalf(0x0300)==3.0f/(1<<16)), "1");

    // Every normal half comes back the same through float
    i32 bad = 0;
    for (u32 h = 0x0400; h < 0x7c00; h++) {
        bad += gmtohalf(gmhalf((u16)h)) != h;
        bad += gmtohalf(gmhalf((u16)(h|0x8000))) != (h|0x8000);
    }
    expect(t, "gemma.half.roundtrip", istr(&a, bad), "0");

    // Halfway goes to even
    f32 ulp = 1.0f/(1<<10);
    expect(t, "gemma.half.tieeven", istr(&a, gmtohalf(1 + ulp/2)), istr(&a, 0x3c00));
    expect(t, "gemma.half.tieodd", istr(&a, gmtohalf(1 + 3*ulp/2)), istr(&a, 0x3c02));
    expect(t, "gemma.half.above", istr(&a, gmtohalf(1 + ulp/2 + ulp/64)), istr(&a, 0x3c01));

    f32 in[]   = {0.5f, 1.5f, 2.5f, -0.5f, -1.5f, -2.5f, 126.6f, -126.4f, 3.0f};
    i32 want[] = {0, 2, 2, 0, -2, -2, 127, -126, 3};
    bad = 0;
    for (i32 i = 0; i < countof(in); i++) {
        bad += (i32)gmrint(in[i]) != want[i];
    }
    expect(t, "gemma.rint", istr(&a, bad), "0");
}

// Plain q4_0 blocks for rows by cols, random.
static u8 *gmtq4(Arena *a, u64 *rng, i32 rows, i32 cols)
{
    iz  n   = (iz)rows*cols/32;
    u8 *src = alloc<u8>(a, n*18);
    for (iz i = 0; i < n; i++) {
        u16 step = gmtohalf(0.001f + 0.02f*gmabs(gmtunit(rng)));
        src[i*18]   = (u8)step;
        src[i*18+1] = (u8)(step>>8);
        for (i32 j = 0; j < 16; j++) {
            src[i*18+2+j] = (u8)gmtrand(rng);
        }
    }
    return src;
}

static void test_gemmakernels(Test *t)
{
    Arena  a   = t->perm;
    u64    rng = 1;
    i32    isa = gmisa(1);
    GmKern ref = gmkern(GM_SCALAR);
    GmKern k   = gmkern(isa);

    // Inputs for up to 7 tokens: a whole tile and a partial one
    enum { ROWS = 48, COLS = 256, NTOK = 7, LDO = ROWS + 5 };
    i8  *in   = alloc<i8>(&a, NTOK*COLS);
    f32 *insc = alloc<f32>(&a, NTOK*COLS/64);
    for (i32 i = 0; i < NTOK*COLS; i++) {
        in[i] = (i8)(gmtrand(&rng)%255 - 127);
    }
    for (i32 i = 0; i < NTOK*COLS/64; i++) {
        insc[i] = 0.01f + gmabs(gmtunit(&rng));
    }

    // The q4 layout, by the plain kernel, against the plain blocks
    u8   *src = gmtq4(&a, &rng, ROWS, COLS);
    GmMat q4  = gmq4mat(&a, ROWS, COLS);
    for (iz g = 0; g < ROWS/8; g++) {
        gmpackq4(&q4, src, g);
    }
    f32 *want = alloc<f32>(&a, NTOK*LDO);
    f32 *got  = alloc<f32>(&a, NTOK*LDO);
    for (i32 tk = 0; tk < NTOK; tk++) {
        for (i32 r = 0; r < ROWS; r++) {
            f32 sum = 0;
            for (i32 b = 0; b < COLS/32; b++) {
                u8 *blk = src + ((iz)r*(COLS/32) + b)*18;
                i32 dot = 0;
                for (i32 j = 0; j < 32; j++) {
                    i32 q = j<16 ? (blk[2+j] & 15) - 8 : (blk[2+j-16] >> 4) - 8;
                    dot += q * in[tk*COLS + b*32 + j];
                }
                f32 step = gmhalf((u16)(blk[0] | blk[1]<<8));
                sum += (f32)dot * (step * insc[tk*(COLS/64) + b/2]);
            }
            want[tk*LDO + r] = sum;
        }
    }
    ref.q4(&q4, got, LDO, in, insc, NTOK, 0, ROWS/8);
    b32 same = 1;
    for (i32 tk = 0; tk < NTOK; tk++) {
        for (i32 r = 0; r < ROWS; r++) {
            same &= got[tk*LDO + r] == want[tk*LDO + r];
        }
    }
    expect(t, "gemma.q4.layout", istr(&a, same), "1");

    // This processor's q4 kernel, on part of the groups: the rest of
    // the output, and the gap between rows, left alone
    for (i32 n = 1; n <= NTOK; n++) {
        for (i32 i = 0; i < NTOK*LDO; i++) {
            got[i] = -99;
        }
        k.q4(&q4, got, LDO, in, insc, n, 1, ROWS/8);
        b32 ok = 1, kept = 1;
        for (i32 tk = 0; tk < NTOK; tk++) {
            for (i32 r = 0; r < LDO; r++) {
                b32 done = tk<n && r>=8 && r<ROWS;
                if (done) {
                    ok &= gmtclose(want + tk*LDO + r, got + tk*LDO + r, 1, 1e-5f) ||
                          gmabs(want[tk*LDO+r] - got[tk*LDO+r]) < 1e-4f;
                } else {
                    kept &= got[tk*LDO + r] == -99;
                }
            }
        }
        Buf b(&a, 32);
        print(&b, "gemma.q4.kernel.");
        print(&b, (i64)n);
        expect(t, finish(&b), istr(&a, ok && kept), "1");
    }

    // The int8 layout: values within half a step of the originals
    GmMat i8m = {};
    i8m.rows   = ROWS;
    i8m.cols   = COLS;
    i8m.type   = GM_I8;
    i8m.data   = alloc<u8>(&a, ROWS*COLS);
    i8m.scales = alloc<u16>(&a, ROWS*COLS/64);
    f32 *orig  = alloc<f32>(&a, ROWS*COLS);
    for (i32 r = 0; r < ROWS; r++) {
        for (i32 c = 0; c < COLS; c++) {
            orig[r*COLS + c] = gmtunit(&rng) * (1 + (f32)(c/64));
        }
        gmpacki8row(&i8m, orig + r*COLS, r);
    }
    i32 far = 0;
    for (i32 r = 0; r < ROWS; r++) {
        for (i32 c = 0; c < COLS; c++) {
            iz  blk  = r/16, inr = r%16, g = c/64, j = c%64;
            f32 step = gmhalf(i8m.scales[(blk*(COLS/64) + g)*16 + inr]);
            i8  q    = ((i8 *)i8m.data)[blk*16*COLS + g*1024 + (j/4)*64 + inr*4 + j%4];
            far += gmabs((f32)q*step - orig[r*COLS + c]) > step*0.5001f;
        }
    }
    expect(t, "gemma.i8.layout", istr(&a, far), "0");

    // This processor's int8 kernel against the plain one
    f32 *ref8 = alloc<f32>(&a, NTOK*LDO);
    for (i32 n = 1; n <= NTOK; n += 3) {
        for (i32 i = 0; i < NTOK*LDO; i++) {
            got[i] = ref8[i] = -99;
        }
        ref.i8(&i8m, ref8, LDO, in, insc, n, 1, ROWS/16);
        k.i8(&i8m, got, LDO, in, insc, n, 1, ROWS/16);
        b32 ok = gmtclose(ref8, got, NTOK*LDO, 1e-5f);
        Buf b(&a, 32);
        print(&b, "gemma.i8.kernel.");
        print(&b, (i64)n);
        expect(t, finish(&b), istr(&a, ok), "1");
    }

    // Attention over a ring, wrapping, and over a strided cache
    enum { HD = 256, RING = 64, STRIDE = 3*HD };
    f32 *q    = alloc<f32>(&a, HD);
    f32 *keys = alloc<f32>(&a, RING*STRIDE);
    for (i32 i = 0; i < HD; i++) {
        q[i] = gmtunit(&rng);
    }
    for (i32 i = 0; i < RING*STRIDE; i++) {
        keys[i] = gmtunit(&rng);
    }
    for (i32 trial = 0; trial < 2; trial++) {
        i32 first  = trial ? 50 : 3;     // 50 wraps past 63
        i32 n      = trial ? 30 : 40;
        i32 mask   = trial ? RING-1 : -1;
        iz  stride = trial ? HD : STRIDE;
        f32 s1[64], s2[64];
        ref.scores(s1, q, keys, first, n, mask, HD, stride);
        k.scores(s2, q, keys, first, n, mask, HD, stride);
        Buf b(&a, 32);
        print(&b, "gemma.scores.");
        print(&b, (i64)trial);
        expect(t, finish(&b), istr(&a, gmtclose(s1, s2, n, 1e-5f)), "1");

        f32 *m1 = alloc<f32>(&a, HD);
        f32 *m2 = alloc<f32>(&a, HD);
        ref.mix(m1, s1, keys + HD, first, n, mask, HD, stride);
        k.mix(m2, s1, keys + HD, first, n, mask, HD, stride);
        b = Buf(&a, 32);
        print(&b, "gemma.mix.");
        print(&b, (i64)trial);
        expect(t, finish(&b), istr(&a, gmtclose(m1, m2, HD, 1e-5f)), "1");
    }
}

// Tiled attention, a tile at a time and in pieces, against plain
// attention in doubles: a full layer across key blocks, and a sliding
// one at its window's edge, wrapping around its ring.
static void test_gemmaattend(Test *t)
{
    Arena a   = t->perm;
    u64   rng = 7;
    Gemma *g  = alloc<Gemma>(&a);
    g->k      = gmkern(gmisa(1));
    GmWork w  = {};
    w.n       = 1;
    w.scratch = alloc<f32>(&a, GM_SCRATCH);

    for (i32 trial = 0; trial < 2; trial++) {
        b32 full   = !trial;
        i32 hd     = full ? GM_FULLHD : GM_SLIDEHD;
        i32 qw     = GM_HEADS*hd;
        i32 n      = full ? 9 : 7;
        i32 pos    = full ? 70 : GM_RING - 3;
        i32 mask   = full ? -1 : GM_RING-1;
        iz  stride = full ? 2*hd : hd;
        iz  cache  = full ? (iz)(pos+n)*stride : 2*(iz)GM_RING*hd;
        f32 *keys  = alloc<f32>(&a, cache);
        f32 *vals  = keys + (full ? hd : (iz)GM_RING*hd);
        for (iz i = 0; i < cache; i++) {
            keys[i] = gmtunit(&rng) * 2;
        }
        g->aux    = alloc<f32>(&a, (iz)n*qw);
        g->hidden = alloc<f32>(&a, (iz)n*qw);
        for (iz i = 0; i < (iz)n*qw; i++) {
            g->aux[i] = gmtunit(&rng);
        }
        GmLayer l = {};
        l.hd   = hd;
        l.full = full;
        GmFwd f = {};
        f.g    = g;
        f.pos  = pos;
        f.ntok = n;

        // Tiles of 4 tokens and all heads (the last one partial), then
        // the first token again a head at a time
        for (i32 t0 = 0; t0 < n; t0 += GM_TILE) {
            i32 t1 = t0+GM_TILE<n ? t0+GM_TILE : n;
            gmattendtile(&w, &f, &l, keys, vals, mask, stride, t0, t1, 0, GM_HEADS);
        }
        f32 *whole = alloc<f32>(&a, qw);
        copybytes(whole, g->hidden, qw*(iz)sizeof(f32));
        for (i32 h = 0; h < GM_HEADS; h++) {
            gmattendtile(&w, &f, &l, keys, vals, mask, stride, 0, 1, h, h+1);
        }
        b32 same = gmtclose(whole, g->hidden, qw, 1e-6f);

        f64 *sc  = alloc<f64>(&a, pos+n);
        f32 *ref = alloc<f32>(&a, (iz)n*qw);
        for (i32 tk = 0; tk < n; tk++) {
            i32 p     = pos + tk;
            i32 first = !full && p+1>GM_WINDOW ? p+1-GM_WINDOW : 0;
            for (i32 h = 0; h < GM_HEADS; h++) {
                f32 *q   = g->aux + (iz)tk*qw + h*hd;
                f64  top = -1e300, sum = 0;
                for (i32 i = first; i <= p; i++) {
                    f32 *key = keys + (iz)(i & mask)*stride;
                    f64  dot = 0;
                    for (i32 j = 0; j < hd; j++) {
                        dot += (f64)q[j] * key[j];
                    }
                    sc[i] = dot;
                    top   = dot>top ? dot : top;
                }
                for (i32 i = first; i <= p; i++) {
                    sc[i] = __builtin_exp(sc[i] - top);
                    sum  += sc[i];
                }
                for (i32 j = 0; j < hd; j++) {
                    f64 v = 0;
                    for (i32 i = first; i <= p; i++) {
                        v += sc[i] * vals[(iz)(i & mask)*stride + j];
                    }
                    ref[(iz)tk*qw + h*hd + j] = (f32)(v / sum);
                }
            }
        }
        Buf b(&a, 32);
        print(&b, "gemma.attend.");
        print(&b, full ? Str("full") : Str("sliding"));
        expect(t, finish(&b), istr(&a, same && gmtclose(ref, g->hidden, (iz)n*qw, 1e-5f)), "1");
    }
}

// GGUF strings, as a tokenizer's arrays hold them
static void gmtstr(Buf *b, Str s)
{
    u64 len = (u64)s.len;
    print(b, {(u8 *)&len, 8});
    print(b, s);
}

static GgArray gmtarray(Str data, u32 type, u64 count)
{
    GgArray r = {};
    r.type  = type;
    r.count = count;
    r.at    = data.data;
    r.end   = data.data + data.len;
    return r;
}

// Encode with specials allowed everywhere (or nowhere), as ids.
static Str gmtencode(Arena *a, GmTok *tok, Str text, b32 specials)
{
    u8 *allow = 0;
    if (specials) {
        allow = alloc<u8>(a, text.len);
        for (iz i = 0; i < text.len; i++) {
            allow[i] = 1;
        }
    }
    Arena      scratch = carve(a, 1<<16);
    Slice<i32> ids = gmencode(tok, a, scratch, text, allow);
    Buf b(a, 64);
    for (iz i = 0; i < ids.len; i++) {
        print(&b, i ? Str(" ") : Str(""));
        print(&b, (i64)ids[i]);
    }
    return finish(&b);
}

static void test_gemmatokens(Test *t)
{
    Arena a = t->perm;

    // A vocabulary of the right size: control tokens, the byte tokens,
    // a few pieces and what merges make, and the rest unused
    enum { A = 600, B, C, AB, ABC, SP, SPA, AA, CH };
    Buf toks(&a, 1<<22), types(&a, 1<<20);
    for (i32 id = 0; id < GM_VOCAB; id++) {
        Arena tmp = a;
        Str   s   = {};
        i32   ty  = 5;
        if (id == 0) {
            s = "<pad>", ty = 3;
        } else if (id == 1) {
            s = "<eos>", ty = 3;
        } else if (id == 2) {
            s = "<bos>", ty = 3;
        } else if (id == 106) {
            s = "<turn|>", ty = 3;
        } else if (id == CH) {
            s = "<|channel>", ty = 4;
        } else if (id>=238 && id<494) {
            Buf b(&tmp, 8);
            print(&b, "<0x");
            printhex(&b, (u64)(id-238), 2);
            print(&b, ">");
            s = finish(&b), ty = 6;
        } else if (id>=A && id<CH) {
            static Str const p[] = {"a", "b", "c", "ab", "abc", "\xe2\x96\x81",
                                    "\xe2\x96\x81" "a", "aa"};
            s = p[id-A], ty = 1;
        } else {
            Buf b(&tmp, 16);
            print(&b, "<unused");
            print(&b, (i64)id);
            print(&b, ">");
            s = finish(&b);
        }
        gmtstr(&toks, s);
        print(&types, {(u8 *)&ty, 4});
    }
    Buf merges(&a, 256);
    gmtstr(&merges, "a b");
    gmtstr(&merges, "ab c");
    gmtstr(&merges, "\xe2\x96\x81 a");
    gmtstr(&merges, "a a");

    GmTok tok = {};
    Arena scratch = carve(&a, 1<<25);
    b32   ok = gmbuildtok(&tok, &a, scratch, gmtarray(finish(&toks), 8, GM_VOCAB),
                          gmtarray(finish(&types), 5, GM_VOCAB),
                          gmtarray(finish(&merges), 8, 4));
    expect(t, "gemma.tok.build", istr(&a, ok), "1");
    if (!ok) {
        return;
    }

    Str want = istr(&a, ABC);
    expect(t, "gemma.tok.merge", gmtencode(&a, &tok, "abc", 0), want);

    // A space is its own piece, merged with the letter after it
    Buf b(&a, 64);
    print(&b, (i64)ABC);
    print(&b, " ");
    print(&b, (i64)SPA);
    expect(t, "gemma.tok.space", gmtencode(&a, &tok, "abc a", 0), finish(&b));

    // No piece: byte tokens
    b = Buf(&a, 64);
    print(&b, (i64)AB);
    print(&b, " ");
    print(&b, (i64)(238+'d'));
    expect(t, "gemma.tok.bytes", gmtencode(&a, &tok, "abd", 0), finish(&b));

    // A UTF-8 character with no piece, as its bytes
    expect(t, "gemma.tok.utf8", gmtencode(&a, &tok, "\xc3\xa9", 0),
           "433 407");

    // The leftmost of equal merges first
    b = Buf(&a, 64);
    print(&b, (i64)AA);
    print(&b, " ");
    print(&b, (i64)A);
    expect(t, "gemma.tok.leftmost", gmtencode(&a, &tok, "aaa", 0), finish(&b));

    // Control tokens only where allowed, and merges not across them
    b = Buf(&a, 64);
    print(&b, "1 ");
    print(&b, (i64)A);
    print(&b, " 106 ");
    print(&b, (i64)B);
    expect(t, "gemma.tok.special", gmtencode(&a, &tok, "<eos>a<turn|>b", 1), finish(&b));
    expect(t, "gemma.tok.notspecial", gmtencode(&a, &tok, "<eos>", 0),
           "298 339 349 353 300");
    b = Buf(&a, 64);
    print(&b, (i64)CH);
    expect(t, "gemma.tok.visible", gmtencode(&a, &tok, "<|channel>", 1), finish(&b));

    expect(t, "gemma.tok.text", tok.text[SPA], " a");
    expect(t, "gemma.tok.byte", tok.text[238+'d'], "d");
    expect(t, "gemma.tok.control", tok.text[106], "");
}

// A small GGUF file: the header, metadata, and no tensors.
static Str gmtgguf(Arena *a, Str arch)
{
    Buf b(a, 512);
    u32 version = 3, typestr = 8, typeu32 = 4;
    u64 ntensors = 0, nmeta = 2;
    print(&b, "GGUF");
    print(&b, {(u8 *)&version, 4});
    print(&b, {(u8 *)&ntensors, 8});
    print(&b, {(u8 *)&nmeta, 8});
    gmtstr(&b, "general.architecture");
    print(&b, {(u8 *)&typestr, 4});
    gmtstr(&b, arch);
    gmtstr(&b, "gemma4.block_count");
    print(&b, {(u8 *)&typeu32, 4});
    u32 layers = GM_LAYERS;
    print(&b, {(u8 *)&layers, 4});
    return finish(&b);
}

static void test_gemmagguf(Test *t)
{
    Arena   a    = t->perm;
    GmPool *pool = gmpool(0, &a, 4, 64, 0);  // tests have no threads
    expect(t, "gemma.pool.alone", istr(&a, pool->n), "1");

    Arena s    = carve(&a, 1<<20);
    Str   file = gmtgguf(&a, "gemma4");
    expect(t, "gemma.gguf.notgguf",
           gmload(pool, &a, s, "GGML", 32768, GM_SCALAR).err, "not a GGUF file");
    expect(t, "gemma.gguf.short",
           gmload(pool, &a, s, takehead(file, 10), 32768, GM_SCALAR).err,
           "unsupported GGUF file");
    expect(t, "gemma.gguf.other",
           gmload(pool, &a, s, gmtgguf(&a, "llama"), 32768, GM_SCALAR).err,
           "not the Gemma 4 E2B model this program runs");

    // Cut short anywhere, refused without reading past the end
    i32 loaded = 0;
    for (iz n = 0; n <= file.len; n++) {
        Arena tmp  = a;
        Str   part = clone(&tmp, takehead(file, n));
        loaded += !!gmload(pool, &tmp, s, part, 32768, GM_SCALAR).g;
    }
    expect(t, "gemma.gguf.truncated", istr(&a, loaded), "0");

    // Arrays nested past one level, and counts past the file
    Buf b(&a, 128);
    u32 version = 3, typearr = 9;
    u64 zero = 0, one = 1, huge = (u64)1<<60;
    print(&b, "GGUF");
    print(&b, {(u8 *)&version, 4});
    print(&b, {(u8 *)&zero, 8});
    print(&b, {(u8 *)&one, 8});
    gmtstr(&b, "k");
    print(&b, {(u8 *)&typearr, 4});
    print(&b, {(u8 *)&typearr, 4});
    print(&b, {(u8 *)&huge, 8});
    print(&b, {(u8 *)&typearr, 4});
    print(&b, {(u8 *)&huge, 8});
    expect(t, "gemma.gguf.nested",
           gmload(pool, &a, s, finish(&b), 32768, GM_SCALAR).err,
           "not the Gemma 4 E2B model this program runs");
}
