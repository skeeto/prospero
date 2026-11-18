// Gemma 4 E2B on the CPU: the built-in model's network (builtin.cpp)
//
// A port of gemma4.c (github.com/ryanssenn/gemma4.c, with the GGUF
// loader and q4_0 kernels of skeeto/gemma4.c's gguf branch) for one file,
// Google's quantization-aware q4_0 GGUF of Gemma 4 E2B, refitted to its
// trained grid (wellons/gemma-4-E2B-it-qat-q4_0-refit-GGUF). Loading
// checks the file against this one architecture, rearranges its q4_0
// matrices for the kernels (gmpackq4), and converts the token table,
// which is also the output layer, to int8. The per-layer table stays in
// the file, read a row at a time.
//
// The kernels come in three builds, chosen at load by what the processor
// has (gmisa): AVX2 with FMA and F16C on x86-64, NEON with the dot
// product instructions on arm64, and a plain one that the unit tests
// compare them with, too slow for use. A pool of workers runs each pass
// (gmrun), every worker its share of every step, with a barrier between.
//
// Unlike the rest of Prospero, this file is under the MIT license of the
// program it came from, as its end says.

#if defined(__x86_64__)
#  include <immintrin.h>
#  define GM_X86 1
#  define GM_TAVX2 __attribute__((target("avx2,fma,f16c")))
#elif defined(__aarch64__)
#  include <arm_neon.h>
#  define GM_ARM 1
#  if defined(__clang__)
#    define GM_TDOT __attribute__((target("dotprod")))
#  else
#    define GM_TDOT __attribute__((target("+dotprod")))
#  endif
#endif
#define GM_INLINE __attribute__((always_inline)) inline

using i8 = decltype((signed char)0);

enum {
    GM_LAYERS   = 35,
    GM_WIDTH    = 1536,     // the hidden state
    GM_EVERY    = 5,        // every fifth layer attends to the whole context
    GM_SETS     = 3,        // layers 4, 9, 14 write the full caches
    GM_OWNKV    = 15,       // layers before this write KV, the rest share it
    GM_SLIDEHD  = 256,      // a sliding layer's head, and its one KV head
    GM_FULLHD   = 512,      // a full layer's
    GM_HEADS    = 8,        // query heads
    GM_FFNMAX   = 8*GM_WIDTH,
    GM_PERLAYER = 256,      // each layer's conditioning input
    GM_VOCAB    = 262144,
    GM_MAXCTX   = 131072,   // the model's own limit
    GM_WINDOW   = 512,      // sliding attention's
    GM_BATCH    = 512,      // positions per pass, at most
    GM_RING     = GM_WINDOW + GM_BATCH,  // a sliding cache's positions
    GM_POSFLOATS = GM_SETS*2*GM_FULLHD,  // a full-cache position: 12 KiB
    GM_BOS      = 2,
    GM_EOS      = 1,
    GM_ENDTURN  = 106,
    GM_TILE     = 4,        // attention's tokens per job, at most
    GM_QTILE    = GM_TILE*GM_HEADS,  // and so its queries
    GM_BLOCK    = 16,       // keys at a time
    // A worker's scratch floats: a row of the model, or attention's
    GM_SCRATCH  = GM_WIDTH > GM_BLOCK+GM_FULLHD+2*GM_QTILE
                ? GM_WIDTH : GM_BLOCK+GM_FULLHD+2*GM_QTILE,
};

enum {
    GM_SCALAR,  // the reference, for tests
    GM_AVX2,
    GM_NEONDOT,
};

// A matrix of rows (outputs) by cols (inputs): q4_0 rearranged by
// gmpackq4, or int8 by gmpacki8. A vector is f32 and rows long.
enum { GM_Q4, GM_I8 };

struct GmMat {
    u8  *data;
    u16 *scales;
    i32  rows;
    i32  cols;
    i32  type;
};

struct GmLayer {
    f32   *attnnorm;
    f32   *postattn;
    f32   *ffnnorm;
    f32   *postffn;
    f32   *postpl;
    f32   *qnorm;
    f32   *knorm;
    f32    scalar;
    i32    hd;      // head width: GM_SLIDEHD or GM_FULLHD
    b32    full;
    GmMat  q, k, v, o;
    GmMat  gate, up, down;
    GmMat  plgate, plproj;
};

struct GmSpecial {
    Str text;
    i32 id;
};

struct GmTok {
    Str       *text;      // each token's text as printed, specials empty
    GmSpecial *specials;  // longest first
    i32        nspecials;
    u64       *piecekey;  // pieces of up to 4 bytes no merge makes
    i32       *pieceid;
    i32        pieceexp;
    u64       *mergekey;  // left<<32 | right, with bit 63 set
    i32       *mergeto;
    i32       *mergerank;
    i32        mergeexp;
};

struct GmPool;

// One worker's part in a pass: its index of n, and scratch of its own.
struct GmWork {
    GmPool *pool;
    f32    *scratch;
    i32     id;
    i32     n;
    i32     seen;  // the last pass run
};

struct GmPool {
    Os      *os;
    void   (*fn)(void *, GmWork *);
    void    *arg;
    GmWork  *work;
    Event   *wake;    // for each worker but the first, and asleep
    i32     *asleep;  // in gmbarrier, to be woken
    void   (*onstart)(Os *);  // run by each worker as it starts
    i64      spinus;
    i32      n;
    i32      gen;
    i32      arrive;
    i32      sense;
};

struct GmKern {
    // out[t*ldo + g*8 ...] for the 8-row groups g0 to g1
    void (*q4)(GmMat *, f32 *out, iz ldo, i8 *in, f32 *insc, i32 ntok,
               iz g0, iz g1);
    // out[t*ldo + b*16 ...] for the 16-row blocks b0 to b1
    void (*i8)(GmMat *, f32 *out, iz ldo, i8 *in, f32 *insc, i32 ntok,
               iz b0, iz b1);
    // scores[i] = q·key(first+i), keys at (pos & mask)*stride
    void (*scores)(f32 *scores, f32 *q, f32 *keys, i32 first, i32 n,
                   i32 mask, i32 hd, iz stride);
    // out = Σ p[i]·value(first+i)
    void (*mix)(f32 *out, f32 *p, f32 *vals, i32 first, i32 n,
                i32 mask, i32 hd, iz stride);
};

// The caches for one sequence: its tokens, each sliding layer's ring of
// the newest GM_RING positions, then the full layers' keys and values, a
// position at a time (GM_POSFLOATS), committed as the sequence grows.
struct GmKv {
    Region *region;
    i32    *tokens;
    f32    *ring;
    f32    *full;
    i32     cap;   // positions the full caches can hold
};

enum : iz {
    GM_RINGFLOATS = (iz)GM_SETS*(GM_EVERY-1)*2*GM_RING*GM_SLIDEHD,  // 25 MB
};

struct Gemma {
    GmLayer layers[GM_LAYERS];
    GmMat   embed;      // int8; the output layer too
    u8     *plembed;    // q6_K rows in the file, GM_LAYERS*GM_PERLAYER wide
    GmMat   plmodel;    // int8
    f32    *plnorm;
    f32    *norm;
    f32    *ropecos[2];  // [full]: per position, pairs[full] each
    f32    *ropesin[2];
    i32     pairs[2];
    f32    *gelu;       // GM_GELUN samples from -8 to 8
    GmTok   tok;
    GmKern  k;
    GmPool *pool;
    i32     isa;
    i32     ctx;        // positions the rope tables cover
    void  (*tick)(void *);  // after each layer, on worker 0, if set
    void   *tickctx;

    // A pass's buffers
    f32    *residual;   // GM_BATCH x GM_WIDTH
    f32    *hidden;     // GM_BATCH x GM_FFNMAX
    f32    *aux;
    i8     *quant;
    f32    *qscale;     // one per 64 of quant
    f32    *plin;       // GM_BATCH x GM_LAYERS x GM_PERLAYER
    f32    *logits;     // GM_VOCAB
};

enum {
    GM_GELUN = 4096,
    GM_SPINUS = 200,   // a barrier's spinning before it sleeps, in µs
};


// Numbers

static f32 gmhalf(u16 h)  // IEEE half to float
{
    u32 s = (u32)(h>>15) << 31;
    u32 e = h>>10 & 31;
    u32 m = h & 1023;
    u32 bits = 0;
    if (e == 31) {
        bits = s | 0x7f800000u | m<<13;
    } else if (e) {
        bits = s | (e + 112)<<23 | m<<13;
    } else if (!m) {
        bits = s;
    } else {
        for (e = 113; !(m & 1024); e--) {
            m <<= 1;
        }
        bits = s | e<<23 | (m & 1023)<<13;
    }
    f32 f;
    copybytes(&f, &bits, 4);
    return f;
}

// A float in the normal half range to the nearest half, ties to even.
static u16 gmtohalf(f32 f)
{
    u32 bits;
    copybytes(&bits, &f, 4);
    u32 s = bits>>16 & 0x8000;
    i32 e = (i32)(bits>>23 & 255) - 112;
    u32 m = bits & 0x7fffff;
    if (e <= 0) {
        return (u16)s;  // below the normal range: not asked for
    } else if (e >= 31) {
        return (u16)(s | 0x7c00);
    }
    u32 h = (u32)e<<10 | m>>13;
    u32 rest = m & 0x1fff;
    h += rest>0x1000 || (rest==0x1000 && (h & 1));  // may carry to inf
    return (u16)(s | h);
}

// The nearest integer, ties to even, as rintf does in the default
// rounding mode, for |x| below 2^22, without a library call.
static f32 gmrint(f32 x)
{
    f32 big = 12582912.0f;  // 1.5 * 2^23
    return x<0 ? (x - big) + big : (x + big) - big;
}

static f32 gmabs(f32 x)
{
    return x<0 ? -x : x;
}

static void gmpause()
{
    #if GM_X86
    __builtin_ia32_pause();
    #elif GM_ARM
    __asm__ volatile ("yield");
    #endif
}


// Workers

struct GmSpan {
    iz beg;
    iz end;
};

// This worker's share of total items.
static GmSpan gmspan(GmWork *w, iz total)
{
    GmSpan r = {};
    r.beg = total * w->id / w->n;
    r.end = total * (w->id+1) / w->n;
    return r;
}

// Wait for every worker to arrive. A waiter spins a while, then sleeps
// on its event: spinning alone, a worker the system has put aside for
// another thread (the window's, the engine's) holds up every other for
// the rest of its time slice. The waiter's flag and the releaser's
// sense are each stored before the other is loaded, sequentially
// consistent, so one of them always sees the other's: no lost wakeup.
// A wakeup can be left over, so every wait checks its condition again.
static void gmbarrier(GmWork *w)
{
    GmPool *p = w->pool;
    if (p->n == 1) {
        return;
    }
    i32 sense = __atomic_load_n(&p->sense, __ATOMIC_RELAXED);
    if (__atomic_add_fetch(&p->arrive, 1, __ATOMIC_ACQ_REL) == p->n) {
        __atomic_store_n(&p->arrive, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&p->sense, sense+1, __ATOMIC_SEQ_CST);
        for (i32 i = 0; i < p->n; i++) {
            if (__atomic_load_n(&p->asleep[i], __ATOMIC_SEQ_CST)) {
                __atomic_store_n(&p->asleep[i], 0, __ATOMIC_RELAXED);
                os_signal(&p->wake[i]);
            }
        }
        return;
    }
    i64 start = 0;
    for (i32 i = 1;; i++) {
        if (__atomic_load_n(&p->sense, __ATOMIC_ACQUIRE) != sense) {
            return;
        } else if (!(i & 255)) {
            i64 now = os_ticks(p->os);
            start = start ? start : now;
            if (now-start > p->spinus) {
                break;
            }
        }
        gmpause();
    }
    for (;;) {
        __atomic_store_n(&p->asleep[w->id], 1, __ATOMIC_SEQ_CST);
        if (__atomic_load_n(&p->sense, __ATOMIC_SEQ_CST) != sense) {
            __atomic_store_n(&p->asleep[w->id], 0, __ATOMIC_RELAXED);
            return;
        }
        os_wait(&p->wake[w->id], -1);
        if (__atomic_load_n(&p->sense, __ATOMIC_ACQUIRE) != sense) {
            return;
        }
    }
}

static void gmworker(void *arg)
{
    GmWork *w = (GmWork *)arg;
    GmPool *p = w->pool;
    if (p->onstart) {
        p->onstart(p->os);
    }
    for (;;) {
        // The pass's signal may have been taken by the last barrier's
        // wait, coalesced with its wakeup: look before waiting.
        i32 gen = __atomic_load_n(&p->gen, __ATOMIC_ACQUIRE);
        if (gen == w->seen) {
            os_wait(&p->wake[w->id], -1);
            continue;
        }
        w->seen = gen;
        p->fn(p->arg, w);
        gmbarrier(w);
    }
}

// A pool of up to n workers, the caller being the first, each with
// scratch floats of its own. Fewer if threads can't be had, down to
// the caller alone, which runs every pass by itself.
static GmPool *gmpool(Os *os, Arena *perm, i32 n, iz scratch,
                      void (*onstart)(Os *))
{
    n = n<1 ? 1 : n>256 ? 256 : n;
    GmPool *p = alloc<GmPool>(perm);
    p->os      = os;
    p->onstart = onstart;
    p->work    = alloc<GmWork>(perm, n);
    p->wake    = alloc<Event>(perm, n);
    p->asleep  = alloc<i32>(perm, n);
    p->spinus  = GM_SPINUS;
    for (i32 i = 0; i < n; i++) {
        p->work[i].pool    = p;
        p->work[i].id      = i;
        p->work[i].scratch = alloc<f32>(perm, scratch);
    }
    p->n = 1;
    if (n>1 && os_eventinit(&p->wake[0])) {
        for (i32 i = 1; i < n; i++) {
            if (!os_eventinit(&p->wake[i]) || !os_thread(os, gmworker, p->work+i)) {
                break;
            }
            p->n = i + 1;
        }
    }
    for (i32 i = 0; i < p->n; i++) {
        p->work[i].n = p->n;
    }
    return p;
}

// Workers for this many performance cores: one left for the window,
// the engine and the rest of the system, since every barrier waits for
// the slowest worker, and a worker that has to share its core is slow.
static i32 gmthreads(i32 cores)
{
    return cores>2 ? cores-1 : cores;
}

// Run fn on every worker, returning when all are done.
static void gmrun(GmPool *p, void (*fn)(void *, GmWork *), void *arg)
{
    p->fn  = fn;
    p->arg = arg;
    __atomic_store_n(&p->gen, p->gen+1, __ATOMIC_RELEASE);
    for (i32 i = 1; i < p->n; i++) {
        os_signal(&p->wake[i]);
    }
    fn(arg, p->work);
    gmbarrier(p->work);
}


// Kernels: the reference

static void q4scalar(GmMat *m, f32 *out, iz ldo, i8 *in, f32 *insc,
                     i32 ntok, iz g0, iz g1)
{
    iz width = m->cols, blocks = width/32, groups = width/64;
    for (i32 t = 0; t < ntok; t++) {
        for (iz g = g0; g < g1; g++) {
            u8  *packed = m->data + g*blocks*128;
            u16 *scales = m->scales + g*blocks*8;
            f32  sum[8] = {};
            for (iz b = 0; b < blocks; b++) {
                i32 dot[8] = {};
                for (i32 c = 0; c < 8; c++) {
                    for (i32 j = 0; j < 16; j++) {
                        i32 p = packed[b*128 + c*16 + j];
                        i32 x = in[t*width + b*32 + c*4 + j%4];
                        dot[j/4]   += ((p & 15) - 8) * x;
                        dot[4+j/4] += ((p >> 4) - 8) * x;
                    }
                }
                f32 s = insc[t*groups + b/2];
                for (i32 r = 0; r < 8; r++) {
                    sum[r] += (f32)dot[r] * (gmhalf(scales[b*8+r]) * s);
                }
            }
            for (i32 r = 0; r < 8; r++) {
                out[t*ldo + g*8 + r] = sum[r];
            }
        }
    }
}

static void i8scalar(GmMat *m, f32 *out, iz ldo, i8 *in, f32 *insc,
                     i32 ntok, iz b0, iz b1)
{
    iz width = m->cols, groups = width/64;
    for (i32 t = 0; t < ntok; t++) {
        for (iz b = b0; b < b1; b++) {
            i8 *base = (i8 *)m->data + b*16*width;
            f32 sum[16] = {};
            for (iz g = 0; g < groups; g++) {
                f32 s = insc[t*groups + g];
                for (i32 r = 0; r < 16; r++) {
                    i32 dot = 0;
                    for (i32 j = 0; j < 64; j++) {
                        dot += base[g*1024 + (j/4)*64 + r*4 + j%4] *
                               in[t*width + g*64 + j];
                    }
                    sum[r] += (f32)dot * (gmhalf(m->scales[(b*groups+g)*16+r]) * s);
                }
            }
            for (i32 r = 0; r < 16; r++) {
                out[t*ldo + b*16 + r] = sum[r];
            }
        }
    }
}

static void scoresscalar(f32 *scores, f32 *q, f32 *keys, i32 first, i32 n,
                         i32 mask, i32 hd, iz stride)
{
    for (i32 i = 0; i < n; i++) {
        f32 *key = keys + (iz)((first+i) & mask)*stride;
        f32  sum = 0;
        for (i32 j = 0; j < hd; j++) {
            sum += q[j] * key[j];
        }
        scores[i] = sum;
    }
}

static void mixscalar(f32 *out, f32 *p, f32 *vals, i32 first, i32 n,
                      i32 mask, i32 hd, iz stride)
{
    for (i32 j = 0; j < hd; j++) {
        out[j] = 0;
    }
    for (i32 i = 0; i < n; i++) {
        f32 *v = vals + (iz)((first+i) & mask)*stride;
        for (i32 j = 0; j < hd; j++) {
            out[j] += p[i] * v[j];
        }
    }
}


// Kernels: AVX2

#if GM_X86
// maddubs needs one unsigned operand: the weights' magnitudes, with their
// signs moved onto the activations. Its 16-bit sums cannot saturate
// within a block (8 chunks of at most 2 * 8 * 127).
GM_TAVX2 static GM_INLINE void q4avx2group(
    GmMat *m, f32 *out, iz ldo, i8 *in, f32 *insc, iz t0, i32 ntok, iz g)
{
    enum { TILE = 6 };
    iz   width  = m->cols, blocks = width/32, groups = width/64;
    u8  *packed = m->data + g*blocks*128;
    u16 *scales = m->scales + g*blocks*8;
    i8  *input  = in + t0*width;
    f32 *inscale = insc + t0*groups;
    __m256 sum[TILE];
    for (i32 t = 0; t < ntok; t++) {
        sum[t] = _mm256_setzero_ps();
    }
    for (iz b = 0; b < blocks; b++) {
        __m256i dot[TILE];
        for (i32 t = 0; t < ntok; t++) {
            dot[t] = _mm256_setzero_si256();
        }
        for (i32 c = 0; c < 8; c++) {
            __m128i p    = _mm_loadu_si128((__m128i *)(packed + b*128 + c*16));
            __m128i lo   = _mm_and_si128(p, _mm_set1_epi8(15));
            __m128i hi   = _mm_and_si128(_mm_srli_epi16(p, 4), _mm_set1_epi8(15));
            __m256i w    = _mm256_sub_epi8(_mm256_set_m128i(hi, lo), _mm256_set1_epi8(8));
            __m256i size = _mm256_abs_epi8(w);
            for (i32 t = 0; t < ntok; t++) {
                i32 four;
                copybytes(&four, input + t*width + b*32 + c*4, 4);
                __m256i a = _mm256_sign_epi8(_mm256_set1_epi32(four), w);
                dot[t] = _mm256_add_epi16(dot[t], _mm256_maddubs_epi16(size, a));
            }
        }
        __m256 scale = _mm256_cvtph_ps(_mm_loadu_si128((__m128i *)(scales + b*8)));
        for (i32 t = 0; t < ntok; t++) {
            __m256 d = _mm256_cvtepi32_ps(_mm256_madd_epi16(dot[t], _mm256_set1_epi16(1)));
            __m256 s = _mm256_mul_ps(scale, _mm256_set1_ps(inscale[t*groups + b/2]));
            sum[t] = _mm256_fmadd_ps(d, s, sum[t]);
        }
    }
    for (i32 t = 0; t < ntok; t++) {
        _mm256_storeu_ps(out + (t0+t)*ldo + g*8, sum[t]);
    }
}

// Each worker keeps its share of the row groups and walks the tokens a
// tile at a time, so a tile's activations stay in L1 while its weights
// stream from L2.
GM_TAVX2 static void q4avx2(GmMat *m, f32 *out, iz ldo, i8 *in, f32 *insc,
                           i32 ntok, iz g0, iz g1)
{
    i32 t = 0;
    for (; t+6 <= ntok; t += 6) {
        for (iz g = g0; g < g1; g++) {
            q4avx2group(m, out, ldo, in, insc, t, 6, g);
        }
    }
    for (; t < ntok; t++) {
        for (iz g = g0; g < g1; g++) {
            q4avx2group(m, out, ldo, in, insc, t, 1, g);
        }
    }
}

GM_TAVX2 static void i8avx2(GmMat *m, f32 *out, iz ldo, i8 *in, f32 *insc,
                           i32 ntok, iz b0, iz b1)
{
    iz width = m->cols, groups = width/64;
    for (iz blk = b0; blk < b1; blk++) {
        i8 *packed = (i8 *)m->data + blk*16*width;
        for (i32 t0 = 0; t0 < ntok; t0 += 4) {
            i32 rows = ntok-t0<4 ? ntok-t0 : 4;
            for (i32 half = 0; half < 2; half++) {
                __m256 result[4] = {};
                for (iz g = 0; g < groups; g++) {
                    __m256i dot[4] = {};
                    for (i32 c = 0; c < 16; c++) {
                        __m256i w = _mm256_loadu_si256((__m256i *)(packed + g*1024 + c*64 + half*32));
                        __m256i size = _mm256_abs_epi8(w);
                        for (i32 r = 0; r < rows; r++) {
                            i32 four;
                            copybytes(&four, in + (t0+r)*width + g*64 + c*4, 4);
                            __m256i a = _mm256_sign_epi8(_mm256_set1_epi32(four), w);
                            dot[r] = _mm256_add_epi32(dot[r], _mm256_madd_epi16(
                                _mm256_maddubs_epi16(size, a), _mm256_set1_epi16(1)));
                        }
                    }
                    __m256 scale = _mm256_cvtph_ps(_mm_loadu_si128(
                        (__m128i *)(m->scales + (blk*groups + g)*16 + half*8)));
                    for (i32 r = 0; r < rows; r++) {
                        __m256 s = _mm256_mul_ps(scale, _mm256_set1_ps(insc[(t0+r)*groups + g]));
                        result[r] = _mm256_fmadd_ps(_mm256_cvtepi32_ps(dot[r]), s, result[r]);
                    }
                }
                for (i32 r = 0; r < rows; r++) {
                    _mm256_storeu_ps(out + (t0+r)*ldo + blk*16 + half*8, result[r]);
                }
            }
        }
    }
}

GM_TAVX2 static void scoresavx2(f32 *scores, f32 *q, f32 *keys, i32 first,
                               i32 n, i32 mask, i32 hd, iz stride)
{
    for (i32 i = 0; i < n; i++) {
        f32   *key = keys + (iz)((first+i) & mask)*stride;
        __m256 s0  = _mm256_setzero_ps();
        __m256 s1  = _mm256_setzero_ps();
        for (i32 j = 0; j < hd; j += 16) {
            s0 = _mm256_fmadd_ps(_mm256_loadu_ps(q+j),   _mm256_loadu_ps(key+j),   s0);
            s1 = _mm256_fmadd_ps(_mm256_loadu_ps(q+j+8), _mm256_loadu_ps(key+j+8), s1);
        }
        __m256 s8 = _mm256_add_ps(s0, s1);
        __m128 s4 = _mm_add_ps(_mm256_castps256_ps128(s8), _mm256_extractf128_ps(s8, 1));
        s4 = _mm_add_ps(s4, _mm_movehl_ps(s4, s4));
        scores[i] = _mm_cvtss_f32(_mm_add_ss(s4, _mm_movehdup_ps(s4)));
    }
}

GM_TAVX2 static void mixavx2(f32 *out, f32 *p, f32 *vals, i32 first, i32 n,
                            i32 mask, i32 hd, iz stride)
{
    for (i32 j = 0; j < hd; j += 64) {
        __m256 sum[8] = {};
        for (i32 i = 0; i < n; i++) {
            f32   *v = vals + (iz)((first+i) & mask)*stride + j;
            __m256 w = _mm256_set1_ps(p[i]);
            for (i32 u = 0; u < 8; u++) {
                sum[u] = _mm256_fmadd_ps(w, _mm256_loadu_ps(v + u*8), sum[u]);
            }
        }
        for (i32 u = 0; u < 8; u++) {
            _mm256_storeu_ps(out + j + u*8, sum[u]);
        }
    }
}

// x86-64 with AVX2, FMA and F16C, and an OS that saves the AVX state.
static b32 gmhasavx2()
{
    u32 a, b, c, d;
    __asm__ ("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0), "c"(0));
    if (a < 7) {
        return 0;
    }
    __asm__ ("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    u32 need = 1u<<12 | 1u<<27 | 1u<<28 | 1u<<29;  // FMA OSXSAVE AVX F16C
    if ((c & need) != need) {
        return 0;
    }
    u32 lo, hi;
    __asm__ ("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    if ((lo & 6) != 6) {
        return 0;  // XMM and YMM state
    }
    __asm__ ("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(7), "c"(0));
    return (b>>5 & 1);
}
#endif


// Kernels: NEON with the dot product

#if GM_ARM
// Rows 0-3 of a group in the low nibbles, 4-7 in the high, so one
// unpacked vector serves a whole tile of tokens, and each row's sum
// builds up in its own lane.
GM_TDOT static GM_INLINE void q4dotgroup(
    GmMat *m, f32 *out, iz ldo, i8 *in, f32 *insc, iz t0, i32 ntok, iz g)
{
    enum { TILE = 4 };
    iz   width  = m->cols, blocks = width/32, groups = width/64;
    u8  *packed = m->data + g*blocks*128;
    u16 *scales = m->scales + g*blocks*8;
    i8  *input  = in + t0*width;
    f32 *inscale = insc + t0*groups;
    float32x4_t losum[TILE], hisum[TILE];
    for (i32 t = 0; t < ntok; t++) {
        losum[t] = hisum[t] = vdupq_n_f32(0);
    }
    for (iz b = 0; b < blocks; b++) {
        int32x4_t lodot[TILE], hidot[TILE];
        for (i32 t = 0; t < ntok; t++) {
            lodot[t] = hidot[t] = vdupq_n_s32(0);
        }
        int8x16_t lo[8], hi[8];
        for (i32 c = 0; c < 8; c++) {
            uint8x16_t p = vld1q_u8(packed + b*128 + c*16);
            lo[c] = vsubq_s8(vreinterpretq_s8_u8(vandq_u8(p, vdupq_n_u8(15))), vdupq_n_s8(8));
            hi[c] = vsubq_s8(vreinterpretq_s8_u8(vshrq_n_u8(p, 4)), vdupq_n_s8(8));
        }
        for (i32 t = 0; t < ntok; t++) {
            int8x16_t x0 = vld1q_s8(input + t*width + b*32);
            int8x16_t x1 = vld1q_s8(input + t*width + b*32 + 16);
            #define GM_LANE(c, x, lane) \
                lodot[t] = vdotq_laneq_s32(lodot[t], lo[c], x, lane); \
                hidot[t] = vdotq_laneq_s32(hidot[t], hi[c], x, lane);
            GM_LANE(0, x0, 0) GM_LANE(1, x0, 1) GM_LANE(2, x0, 2) GM_LANE(3, x0, 3)
            GM_LANE(4, x1, 0) GM_LANE(5, x1, 1) GM_LANE(6, x1, 2) GM_LANE(7, x1, 3)
            #undef GM_LANE
        }
        float32x4_t loscale = vcvt_f32_f16(vreinterpret_f16_u16(vld1_u16(scales + b*8)));
        float32x4_t hiscale = vcvt_f32_f16(vreinterpret_f16_u16(vld1_u16(scales + b*8 + 4)));
        for (i32 t = 0; t < ntok; t++) {
            f32 s = inscale[t*groups + b/2];
            losum[t] = vfmaq_f32(losum[t], vcvtq_f32_s32(lodot[t]), vmulq_n_f32(loscale, s));
            hisum[t] = vfmaq_f32(hisum[t], vcvtq_f32_s32(hidot[t]), vmulq_n_f32(hiscale, s));
        }
    }
    for (i32 t = 0; t < ntok; t++) {
        f32 *o = out + (t0+t)*ldo + g*8;
        vst1q_f32(o,   losum[t]);
        vst1q_f32(o+4, hisum[t]);
    }
}

GM_TDOT static void q4dot(GmMat *m, f32 *out, iz ldo, i8 *in, f32 *insc,
                         i32 ntok, iz g0, iz g1)
{
    i32 t = 0;
    for (; t+4 <= ntok; t += 4) {
        for (iz g = g0; g < g1; g++) {
            q4dotgroup(m, out, ldo, in, insc, t, 4, g);
        }
    }
    for (; t < ntok; t++) {
        for (iz g = g0; g < g1; g++) {
            q4dotgroup(m, out, ldo, in, insc, t, 1, g);
        }
    }
}

GM_TDOT static void i8dot(GmMat *m, f32 *out, iz ldo, i8 *in, f32 *insc,
                         i32 ntok, iz b0, iz b1)
{
    iz width = m->cols, groups = width/64;
    for (iz blk = b0; blk < b1; blk++) {
        i8 *packed = (i8 *)m->data + blk*16*width;
        for (i32 t0 = 0; t0 < ntok; t0 += 4) {
            i32 rows = ntok-t0<4 ? ntok-t0 : 4;
            float32x4_t result[4][4];
            for (i32 r = 0; r < 4; r++) {
                for (i32 q = 0; q < 4; q++) {
                    result[r][q] = vdupq_n_f32(0);
                }
            }
            for (iz g = 0; g < groups; g++) {
                int32x4_t dot[4][4];
                for (i32 r = 0; r < 4; r++) {
                    for (i32 q = 0; q < 4; q++) {
                        dot[r][q] = vdupq_n_s32(0);
                    }
                }
                for (i32 c = 0; c < 16; c++) {
                    int8x16_t w[4];
                    for (i32 q = 0; q < 4; q++) {
                        w[q] = vld1q_s8(packed + g*1024 + c*64 + q*16);
                    }
                    for (i32 r = 0; r < rows; r++) {
                        i32 four;
                        copybytes(&four, in + (t0+r)*width + g*64 + c*4, 4);
                        int8x16_t x = vreinterpretq_s8_s32(vdupq_n_s32(four));
                        for (i32 q = 0; q < 4; q++) {
                            dot[r][q] = vdotq_s32(dot[r][q], w[q], x);
                        }
                    }
                }
                u16 *sc = m->scales + (blk*groups + g)*16;
                for (i32 r = 0; r < rows; r++) {
                    float32x4_t s = vdupq_n_f32(insc[(t0+r)*groups + g]);
                    for (i32 q = 0; q < 4; q++) {
                        float32x4_t scale = vcvt_f32_f16(vreinterpret_f16_u16(vld1_u16(sc + q*4)));
                        result[r][q] = vfmaq_f32(result[r][q], vcvtq_f32_s32(dot[r][q]),
                                                 vmulq_f32(scale, s));
                    }
                }
            }
            for (i32 r = 0; r < rows; r++) {
                for (i32 q = 0; q < 4; q++) {
                    vst1q_f32(out + (t0+r)*ldo + blk*16 + q*4, result[r][q]);
                }
            }
        }
    }
}

static void scoresneon(f32 *scores, f32 *q, f32 *keys, i32 first, i32 n,
                       i32 mask, i32 hd, iz stride)
{
    for (i32 i = 0; i < n; i++) {
        f32 *key = keys + (iz)((first+i) & mask)*stride;
        float32x4_t s[4];
        for (i32 u = 0; u < 4; u++) {
            s[u] = vdupq_n_f32(0);
        }
        for (i32 j = 0; j < hd; j += 16) {
            for (i32 u = 0; u < 4; u++) {
                s[u] = vfmaq_f32(s[u], vld1q_f32(q + j + u*4), vld1q_f32(key + j + u*4));
            }
        }
        scores[i] = vaddvq_f32(vaddq_f32(vaddq_f32(s[0], s[1]), vaddq_f32(s[2], s[3])));
    }
}

static void mixneon(f32 *out, f32 *p, f32 *vals, i32 first, i32 n,
                    i32 mask, i32 hd, iz stride)
{
    for (i32 j = 0; j < hd; j += 32) {
        float32x4_t sum[8];
        for (i32 u = 0; u < 8; u++) {
            sum[u] = vdupq_n_f32(0);
        }
        for (i32 i = 0; i < n; i++) {
            f32 *v = vals + (iz)((first+i) & mask)*stride + j;
            float32x4_t w = vdupq_n_f32(p[i]);
            for (i32 u = 0; u < 8; u++) {
                sum[u] = vfmaq_f32(sum[u], w, vld1q_f32(v + u*4));
            }
        }
        for (i32 u = 0; u < 8; u++) {
            vst1q_f32(out + j + u*4, sum[u]);
        }
    }
}
#endif

// The best kernels for this processor, or GM_SCALAR when it has neither
// AVX2 nor, on arm64, the dot product (armdot, which only the platform
// can tell outside macOS).
static i32 gmisa(b32 armdot)
{
    #if GM_X86
    return gmhasavx2() ? GM_AVX2 : GM_SCALAR;
    #elif GM_ARM && defined(__APPLE__)
    return GM_NEONDOT;  // every Apple processor has it
    #elif GM_ARM
    return armdot ? GM_NEONDOT : GM_SCALAR;
    #else
    return GM_SCALAR;
    #endif
}

static GmKern gmkern(i32 isa)
{
    GmKern k = {q4scalar, i8scalar, scoresscalar, mixscalar};
    switch (isa) {
    #if GM_X86
    case GM_AVX2: k = {q4avx2, i8avx2, scoresavx2, mixavx2}; break;
    #endif
    #if GM_ARM
    case GM_NEONDOT: k = {q4dot, i8dot, scoresneon, mixneon}; break;
    #endif
    }
    return k;
}


// Steps of a pass: each worker does its share, then waits for the rest.

// Each row's groups of 64 to int8, with a scale per group.
static void gmquantize(GmWork *w, i8 *q, f32 *scales, f32 *in, iz rows, iz width)
{
    GmSpan s = gmspan(w, rows*(width/64));
    for (iz g = s.beg; g < s.end; g++) {
        f32 *x   = in + g*64;
        f32  max = 0;
        for (i32 j = 0; j < 64; j++) {
            f32 a = gmabs(x[j]);
            max = a>max ? a : max;
        }
        f32 scale = max / 127.0f;
        f32 inv   = scale>0 ? 1.0f/scale : 0.0f;
        for (i32 j = 0; j < 64; j++) {
            q[g*64+j] = (i8)gmrint(x[j] * inv);
        }
        scales[g] = scale;
    }
    gmbarrier(w);
}

// m times the quantized rows, into out with rows ldo apart.
static void gmmatmul(GmWork *w, Gemma *g, GmMat *m, f32 *out, iz ldo, i32 ntok)
{
    if (m->type == GM_Q4) {
        GmSpan s = gmspan(w, m->rows/8);
        g->k.q4(m, out, ldo, g->quant, g->qscale, ntok, s.beg, s.end);
    } else {
        GmSpan s = gmspan(w, m->rows/16);
        g->k.i8(m, out, ldo, g->quant, g->qscale, ntok, s.beg, s.end);
    }
    gmbarrier(w);
}

// Rows of width, stride apart, scaled to unit RMS, then by weight.
static void gmrmsnorm(GmWork *w, f32 *out, f32 *in, f32 *weight, iz width,
                      f32 eps, iz rows, iz stride)
{
    GmSpan s = gmspan(w, rows);
    for (iz r = s.beg; r < s.end; r++) {
        f32 *x = in + r*stride;
        f32 *y = out + r*stride;
        f32  sum = 0;
        for (iz i = 0; i < width; i++) {
            sum += x[i] * x[i];
        }
        f32 inv = 1.0f / __builtin_sqrtf(sum/(f32)width + eps);
        for (iz i = 0; i < width; i++) {
            y[i] = (weight ? weight[i] : 1.0f) * (inv * x[i]);
        }
    }
    gmbarrier(w);
}

static void gmaddscale(GmWork *w, f32 *out, f32 *add, iz count, f32 scale)
{
    GmSpan s = gmspan(w, count);
    for (iz i = s.beg; i < s.end; i++) {
        out[i] = (out[i] + add[i]) * scale;
    }
    gmbarrier(w);
}

// gate = GELU(gate) * up, by the table.
static void gmgeglu(GmWork *w, Gemma *g, f32 *gate, f32 *up, iz rows,
                    iz width, iz upstride)
{
    f32    lo = -8, hi = 8;
    f32    scale = (f32)(GM_GELUN - 1) / (hi - lo);
    GmSpan s = gmspan(w, rows*width);
    for (iz e = s.beg; e < s.end; e++) {
        iz  r = e / width, i = e % width;
        f32 x = gate[e];
        if (x <= lo) {
            x = g->gelu[0];
        } else if (!(x >= hi)) {
            f32 at   = (x - lo) * scale;
            i32 k    = (i32)at;
            f32 frac = at - (f32)k;
            x = g->gelu[k] + frac*(g->gelu[k+1] - g->gelu[k]);
        }
        gate[e] = x * up[r*upstride + i];
    }
    gmbarrier(w);
}

// Rotate each head's channel pairs by its position's angles.
static void gmrope(GmWork *w, Gemma *g, b32 full, f32 *vecs, i32 heads,
                   i32 hd, i32 pos, i32 ntok, iz stride)
{
    i32    pairs = g->pairs[full];
    GmSpan s = gmspan(w, ntok);
    for (iz t = s.beg; t < s.end; t++) {
        f32 *cs = g->ropecos[full] + (pos + t)*pairs;
        f32 *sn = g->ropesin[full] + (pos + t)*pairs;
        for (i32 h = 0; h < heads; h++) {
            f32 *v = vecs + t*stride + h*hd;
            for (i32 j = 0; j < pairs; j++) {
                f32 a = v[j];
                f32 b = v[j + hd/2];
                v[j]        = a*cs[j] - b*sn[j];
                v[j + hd/2] = b*cs[j] + a*sn[j];
            }
        }
    }
    gmbarrier(w);
}

// One row of q6_K super-blocks: 256 values each, as 4 low and 2 high
// bits, with a signed 8-bit scale per 16 values and an f16 scale overall.
static void gmq6k(f32 *out, u8 *row, iz width, f32 mul)
{
    for (iz b = 0; b < width/256; b++) {
        u8 *blk  = row + b*210;
        u8 *lo   = blk;
        u8 *hi   = blk + 128;
        i8 *sc   = (i8 *)(blk + 192);
        f32 d    = gmhalf((u16)(blk[208] | blk[209]<<8)) * mul;
        f32 *y   = out + b*256;
        for (i32 half = 0; half < 2; half++) {
            for (i32 l = 0; l < 32; l++) {
                i32 s = l / 16;
                y[l+ 0] = d * (f32)sc[s+0] * (f32)(((lo[l]    & 15) | ((hi[l]>>0 & 3)<<4)) - 32);
                y[l+32] = d * (f32)sc[s+2] * (f32)(((lo[l+32] & 15) | ((hi[l]>>2 & 3)<<4)) - 32);
                y[l+64] = d * (f32)sc[s+4] * (f32)(((lo[l]    >> 4) | ((hi[l]>>4 & 3)<<4)) - 32);
                y[l+96] = d * (f32)sc[s+6] * (f32)(((lo[l+32] >> 4) | ((hi[l]>>6 & 3)<<4)) - 32);
            }
            y  += 128;
            lo += 64;
            hi += 32;
            sc += 8;
        }
    }
}

// Token rows of the int8 table, scaled by mul.
static void gmembed(GmWork *w, f32 *out, GmMat *m, i32 *tokens, i32 ntok, f32 mul)
{
    iz     width = m->cols, groups = width/64;
    GmSpan s = gmspan(w, ntok);
    for (iz t = s.beg; t < s.end; t++) {
        iz  blk = tokens[t] / 16;
        i32 row = tokens[t] % 16;
        f32 *v  = out + t*width;
        i8  *data = (i8 *)m->data + blk*16*width;
        u16 *sc   = m->scales + blk*groups*16;
        for (iz g = 0; g < groups; g++) {
            f32 scale = gmhalf(sc[g*16 + row]) * mul;
            for (i32 j = 0; j < 64; j++) {
                v[g*64+j] = (f32)data[g*1024 + (j/4)*64 + row*4 + j%4] * scale;
            }
        }
    }
    gmbarrier(w);
}

static void gmplembed(GmWork *w, Gemma *g, f32 *out, i32 *tokens, i32 ntok, f32 mul)
{
    iz     width = GM_LAYERS*GM_PERLAYER;
    iz     bytes = width/256*210;
    GmSpan s = gmspan(w, ntok);
    for (iz t = s.beg; t < s.end; t++) {
        gmq6k(out + t*width, g->plembed + tokens[t]*bytes, width, mul);
    }
    gmbarrier(w);
}


// The forward pass

struct GmFwd {
    Gemma *g;
    GmKv  *kv;
    i32   *tokens;
    i32    ntok;
    i32    pos;
    i32    ticket;  // attention's next job
    i32    row;     // gmlogits: the residual row
};

static f32 *gmkeys(GmKv *kv, i32 owner, b32 full)
{
    if (full) {
        return kv->full + (owner/GM_EVERY)*2*GM_FULLHD;
    }
    iz set = owner/GM_EVERY*(GM_EVERY-1) + owner%GM_EVERY;
    return kv->ring + set*2*GM_RING*GM_SLIDEHD;
}

// Causal attention for tokens t0 to t1 and heads h0 to h1, a block of
// keys at a time for all of them, so that each block is read from memory
// once per tile rather than once per query. The softmax goes online: each
// query keeps its running maximum and sum, and rescales its output.
static void gmattendtile(GmWork *w, GmFwd *f, GmLayer *l, f32 *keys,
                         f32 *vals, i32 mask, iz stride, i32 t0, i32 t1,
                         i32 h0, i32 h1)
{
    Gemma *g   = f->g;
    i32    hd  = l->hd;
    i32    qw  = GM_HEADS*hd;
    i32    nq  = (t1 - t0)*(h1 - h0);
    f32   *p   = w->scratch;
    f32   *tmp = p + GM_BLOCK;
    f32   *max = tmp + GM_FULLHD;
    f32   *sum = max + GM_QTILE;
    assert(nq <= GM_QTILE);

    for (i32 i = 0; i < nq; i++) {
        max[i] = -1e30f;
        sum[i] = 0;
    }
    for (i32 t = t0; t < t1; t++) {
        for (i32 h = h0; h < h1; h++) {
            f32 *out = g->hidden + (iz)t*qw + h*hd;
            for (i32 j = 0; j < hd; j++) {
                out[j] = 0;
            }
        }
    }

    i32 last  = f->pos + t1;  // past the last query's own position
    i32 first = l->full || f->pos+t0+1<=GM_WINDOW ? 0 : f->pos+t0+1-GM_WINDOW;
    for (i32 b0 = first; b0 < last; b0 += GM_BLOCK) {
        i32 b1 = b0+GM_BLOCK<last ? b0+GM_BLOCK : last;
        i32 qi = 0;
        for (i32 t = t0; t < t1; t++) {
            i32 pp  = f->pos + t;
            i32 beg = !l->full && pp+1>GM_WINDOW ? pp+1-GM_WINDOW : 0;
            beg     = beg>b0 ? beg : b0;
            i32 end = pp+1<b1 ? pp+1 : b1;
            for (i32 h = h0; h < h1; h++, qi++) {
                if (beg >= end) {
                    continue;
                }
                i32  count = end - beg;
                f32 *q     = g->aux + (iz)t*qw + h*hd;
                f32 *out   = g->hidden + (iz)t*qw + h*hd;
                g->k.scores(p, q, keys, beg, count, mask, hd, stride);
                f32 top = max[qi];
                for (i32 i = 0; i < count; i++) {
                    top = p[i]>top ? p[i] : top;
                }
                f32 scale = __builtin_expf(max[qi] - top);
                f32 add   = 0;
                for (i32 i = 0; i < count; i++) {
                    p[i] = __builtin_expf(p[i] - top);
                    add += p[i];
                }
                max[qi] = top;
                sum[qi] = sum[qi]*scale + add;
                g->k.mix(tmp, p, vals, beg, count, mask, hd, stride);
                for (i32 j = 0; j < hd; j++) {
                    out[j] = out[j]*scale + tmp[j];
                }
            }
        }
    }

    i32 qi = 0;
    for (i32 t = t0; t < t1; t++) {
        for (i32 h = h0; h < h1; h++, qi++) {
            f32 *out = g->hidden + (iz)t*qw + h*hd;
            f32  inv = 1 / sum[qi];
            for (i32 j = 0; j < hd; j++) {
                out[j] *= inv;
            }
        }
    }
}

// Queries, this layer's keys and values if it has its own, and causal
// attention over the window or the whole context, into g->hidden.
static void gmattend(GmWork *w, GmFwd *f, i32 layer)
{
    Gemma   *g  = f->g;
    GmLayer *l  = g->layers + layer;
    i32      hd = l->hd;
    i32      qw = GM_HEADS*hd;
    i32      n  = f->ntok;
    // Shared layers reuse the latest cache of their kind: 13's or 14's
    i32      owner = layer<GM_OWNKV ? layer : GM_OWNKV - 1 - !l->full;
    f32     *keys  = gmkeys(f->kv, owner, l->full);
    iz       stride = l->full ? GM_POSFLOATS : GM_SLIDEHD;
    f32     *vals  = keys + (l->full ? GM_FULLHD : GM_RING*GM_SLIDEHD);
    i32      mask  = l->full ? -1 : GM_RING-1;

    if (!w->id) {
        __atomic_store_n(&f->ticket, 0, __ATOMIC_RELAXED);
    }
    gmquantize(w, g->quant, g->qscale, g->hidden, n, GM_WIDTH);
    gmmatmul(w, g, &l->q, g->aux, qw, n);
    gmrmsnorm(w, g->aux, g->aux, l->qnorm, hd, 1e-6f, (iz)n*GM_HEADS, hd);
    gmrope(w, g, l->full, g->aux, GM_HEADS, hd, f->pos, n, qw);

    if (l->k.data) {
        f32 *k = keys + (iz)(f->pos & mask)*stride;
        f32 *v = vals + (iz)(f->pos & mask)*stride;
        gmmatmul(w, g, &l->k, k, stride, n);
        gmmatmul(w, g, &l->v, v, stride, n);
        gmrmsnorm(w, k, k, l->knorm, hd, 1e-6f, n, stride);
        gmrmsnorm(w, v, v, 0, hd, 1e-6f, n, stride);
        gmrope(w, g, l->full, k, 1, hd, f->pos, n, stride);
    }

    // Jobs are tiles of tokens by heads, the latest tokens (with the most
    // keys) first. Small batches go a query at a time, for parallelism.
    i32 tt    = n / (2*w->n);
    tt        = tt<1 ? 1 : tt>GM_TILE ? GM_TILE : tt;
    i32 hh    = tt>1 ? GM_HEADS : 1;
    i32 tiles = (n + tt - 1) / tt;
    i32 per   = GM_HEADS / hh;
    for (;;) {
        i32 job = __atomic_fetch_add(&f->ticket, 1, __ATOMIC_RELAXED);
        if (job >= tiles*per) {
            break;
        }
        i32 t0 = (tiles - 1 - job/per) * tt;
        i32 t1 = t0+tt<n ? t0+tt : n;
        i32 h0 = job%per * hh;
        gmattendtile(w, f, l, keys, vals, mask, stride, t0, t1, h0, h0+hh);
    }
    gmbarrier(w);

    gmquantize(w, g->quant, g->qscale, g->hidden, n, qw);
    gmmatmul(w, g, &l->o, g->hidden, GM_WIDTH, n);
}

static void gmforwardpass(void *arg, GmWork *w)
{
    GmFwd *f = (GmFwd *)arg;
    Gemma *g = f->g;
    i32    n = f->ntok;
    iz     plwidth = GM_LAYERS*GM_PERLAYER;

    gmembed(w, g->residual, &g->embed, f->tokens, n, __builtin_sqrtf(GM_WIDTH));

    // Each layer's conditioning input, from the token and its embedding
    gmquantize(w, g->quant, g->qscale, g->residual, n, GM_WIDTH);
    gmmatmul(w, g, &g->plmodel, g->plin, plwidth, n);
    gmrmsnorm(w, g->plin, g->plin, g->plnorm, GM_PERLAYER, 1e-6f*(f32)GM_WIDTH,
              (iz)n*GM_LAYERS, GM_PERLAYER);
    gmplembed(w, g, g->hidden, f->tokens, n, __builtin_sqrtf(GM_PERLAYER));
    gmaddscale(w, g->plin, g->hidden, (iz)n*plwidth, 1/__builtin_sqrtf(2));

    for (i32 i = 0; i < GM_LAYERS; i++) {
        GmLayer *l = g->layers + i;
        iz       ffn = l->gate.rows;

        gmrmsnorm(w, g->hidden, g->residual, l->attnnorm, GM_WIDTH, 1e-6f, n, GM_WIDTH);
        gmattend(w, f, i);
        gmrmsnorm(w, g->hidden, g->hidden, l->postattn, GM_WIDTH, 1e-6f, n, GM_WIDTH);
        gmaddscale(w, g->residual, g->hidden, (iz)n*GM_WIDTH, 1);

        gmrmsnorm(w, g->hidden, g->residual, l->ffnnorm, GM_WIDTH, 1e-6f, n, GM_WIDTH);
        gmquantize(w, g->quant, g->qscale, g->hidden, n, GM_WIDTH);
        gmmatmul(w, g, &l->gate, g->hidden, ffn, n);
        gmmatmul(w, g, &l->up, g->aux, ffn, n);
        gmgeglu(w, g, g->hidden, g->aux, n, ffn, ffn);
        gmquantize(w, g->quant, g->qscale, g->hidden, n, ffn);
        gmmatmul(w, g, &l->down, g->hidden, GM_WIDTH, n);
        gmrmsnorm(w, g->hidden, g->hidden, l->postffn, GM_WIDTH, 1e-6f, n, GM_WIDTH);
        gmaddscale(w, g->residual, g->hidden, (iz)n*GM_WIDTH, 1);

        // This layer's conditioning input, gated, into the residual
        gmquantize(w, g->quant, g->qscale, g->residual, n, GM_WIDTH);
        gmmatmul(w, g, &l->plgate, g->hidden, GM_PERLAYER, n);
        gmgeglu(w, g, g->hidden, g->plin + i*GM_PERLAYER, n, GM_PERLAYER, plwidth);
        gmquantize(w, g->quant, g->qscale, g->hidden, n, GM_PERLAYER);
        gmmatmul(w, g, &l->plproj, g->hidden, GM_WIDTH, n);
        gmrmsnorm(w, g->hidden, g->hidden, l->postpl, GM_WIDTH, 1e-6f, n, GM_WIDTH);
        gmaddscale(w, g->residual, g->hidden, (iz)n*GM_WIDTH, l->scalar);
        if (!w->id && g->tick) {
            g->tick(g->tickctx);  // the others go on to the next barrier
        }
    }
}

// Make room in the full caches for positions up to end. 0 if the
// system has no memory for it.
static b32 gmkvgrow(GmKv *kv, i32 end)
{
    iz need = (byte *)(kv->full + (iz)end*GM_POSFLOATS) - kv->region->base;
    return os_commit(kv->region, need);
}

// Run tokens at positions pos onward, at most GM_BATCH, and within
// one turn of the sliding rings (the caller keeps to the GM_BATCH grid).
static b32 gmforward(Gemma *g, GmKv *kv, i32 *tokens, i32 ntok, i32 pos)
{
    assert(ntok>0 && ntok<=GM_BATCH && pos>=0);
    assert(pos%GM_BATCH + ntok <= GM_BATCH);
    assert(pos+ntok <= kv->cap && pos+ntok <= g->ctx);
    for (i32 i = 0; i < ntok; i++) {
        assert(tokens[i]>=0 && tokens[i]<GM_VOCAB);
    }
    if (!gmkvgrow(kv, pos+ntok)) {
        return 0;
    }
    GmFwd f = {};
    f.g      = g;
    f.kv     = kv;
    f.tokens = tokens;
    f.ntok   = ntok;
    f.pos    = pos;
    gmrun(g->pool, gmforwardpass, &f);
    return 1;
}

static void gmlogitspass(void *arg, GmWork *w)
{
    GmFwd *f = (GmFwd *)arg;
    Gemma *g = f->g;
    gmrmsnorm(w, g->hidden, g->residual + (iz)f->row*GM_WIDTH, g->norm,
              GM_WIDTH, 1e-6f, 1, GM_WIDTH);
    gmquantize(w, g->quant, g->qscale, g->hidden, 1, GM_WIDTH);
    gmmatmul(w, g, &g->embed, g->logits, GM_VOCAB, 1);
    GmSpan s = gmspan(w, GM_VOCAB);
    for (iz i = s.beg; i < s.end; i++) {
        g->logits[i] = 30 * __builtin_tanhf(g->logits[i] / 30);  // soft cap
    }
}

// The next token's logits after row (of the last gmforward's tokens).
static f32 *gmlogits(Gemma *g, i32 row)
{
    GmFwd f = {};
    f.g   = g;
    f.row = row;
    gmrun(g->pool, gmlogitspass, &f);
    return g->logits;
}

static i32 gmargmax(f32 *v, i32 n)
{
    i32 best = 0;
    for (i32 i = 1; i < n; i++) {
        best = v[i]>v[best] ? i : best;
    }
    return best;
}

// Caches for a sequence of up to cap positions, or a null region.
static GmKv gmkvnew(Os *os, i32 cap)
{
    GmKv kv   = {};
    iz   toks = ((iz)cap*4 + 63) & -64;
    iz   head = toks + GM_RINGFLOATS*(iz)sizeof(f32);
    kv.region = os_reserve(os, head + (iz)cap*GM_POSFLOATS*(iz)sizeof(f32) + (1<<16));
    if (kv.region) {
        kv.tokens = (i32 *)kv.region->base;
        kv.ring   = (f32 *)(kv.region->base + toks);
        kv.full   = kv.ring + GM_RINGFLOATS;
        kv.cap    = cap;
        if (!os_commit(kv.region, head)) {
            os_release(os, kv.region);
            kv = {};
        }
    }
    return kv;
}


// Tokens

// The slot of key in a table of 2^exp, or the empty one where it goes.
static i32 gmslot(u64 *keys, i32 exp, u64 key)
{
    u64 mask = ((u64)1<<exp) - 1;
    for (u64 i = key*1111111111111111111u >> (64-exp);; i = (i+1) & mask) {
        if (!keys[i] || keys[i]==key) {
            return (i32)i;
        }
    }
}

static u64 gmpiecekey(Str s)  // s.len 1 to 4
{
    u64 k = (u64)s.len << 32;
    for (iz i = 0; i < s.len; i++) {
        k |= (u64)s[i] << (8*i);
    }
    return k;
}

static u64 gmpairkey(i32 a, i32 b)
{
    return (u64)1<<63 | (u64)a<<32 | (u32)b;
}

static i32 gmfindtoken(i32 *slots, i32 exp, Str *texts, Str s)
{
    u64 mask = ((u64)1<<exp) - 1;
    for (u64 i = hash(s) >> (64-exp);; i = (i+1) & mask) {
        i32 id = slots[i];
        if (id<0 || texts[id]==s) {
            return id;
        }
    }
}

static b32 gmspeciallonger(GmSpecial *a, GmSpecial *b)
{
    return a->text.len != b->text.len ? a->text.len>b->text.len
                                      : compare(a->text, b->text) < 0;
}

static i32 gmexp(iz count)  // a table exponent for count entries, half full
{
    i32 exp = 4;
    for (; ((iz)1<<exp) < 2*count; exp++) {}
    return exp;
}

struct GgArray {
    u32 type;
    u64 count;
    u8 *at;
    u8 *end;
};

static Str gmhexbyte(Arena *a, Str s)  // "<0xNN>" to its byte
{
    u8 v = 0;
    for (iz i = 3; i < 5 && i < s.len; i++) {
        u8 c = s[i];
        v = (u8)(v<<4 | (c<='9' ? c-'0' : (c|32)-'a'+10));
    }
    Str r = {alloc<u8>(a), 1};
    r.data[0] = v;
    return r;
}

static Str ggreadstr(u8 **at, u8 *end, b32 *bad);

// Build the tokenizer from the GGUF's arrays, as gemma4.c's exporter
// did: special tokens (control tokens, and the few marked user-defined
// that print), pieces of up to 4 bytes no merge makes, and the merges
// by pair. 0 on failure.
static b32 gmbuildtok(GmTok *t, Arena *perm, Arena scratch, GgArray tokens,
                      GgArray types, GgArray merges)
{
    if (tokens.type!=8 || tokens.count!=GM_VOCAB || types.type!=5 ||
        types.count!=GM_VOCAB || merges.type!=8 || merges.count>(1<<21) ||
        types.end-types.at < 4*GM_VOCAB) {
        return 0;
    }

    Str *raw   = alloc<Str>(&scratch, GM_VOCAB);
    i32  vexp  = gmexp(GM_VOCAB);
    i32 *slots = alloc<i32>(&scratch, (iz)1<<vexp);
    b32  bad   = 0;
    for (iz i = 0; i < (iz)1<<vexp; i++) {
        slots[i] = -1;
    }
    u8 *at = tokens.at;
    for (i32 id = 0; id < GM_VOCAB && !bad; id++) {
        raw[id] = ggreadstr(&at, tokens.end, &bad);
        u64 mask = ((u64)1<<vexp) - 1;
        u64 i = hash(raw[id]) >> (64-vexp);
        for (; slots[i] >= 0; i = (i+1) & mask) {}
        slots[i] = id;
    }
    if (bad) {
        return 0;
    }

    t->mergeexp  = gmexp((iz)merges.count);
    t->mergekey  = alloc<u64>(perm, (iz)1<<t->mergeexp);
    t->mergeto   = alloc<i32>(perm, (iz)1<<t->mergeexp);
    t->mergerank = alloc<i32>(perm, (iz)1<<t->mergeexp);
    u8 *made = alloc<u8>(&scratch, GM_VOCAB);  // a merge makes it
    at = merges.at;
    for (i32 rank = 0; rank < (i32)merges.count; rank++) {
        Str m = ggreadstr(&at, merges.end, &bad);
        if (bad) {
            return 0;
        }
        // Split at whichever space leaves a token on both sides
        i32 left = -1, right = -1, result = -1;
        for (iz i = 0; i < m.len && result<0; i++) {
            if (m[i] != ' ') {
                continue;
            }
            left  = gmfindtoken(slots, vexp, raw, takehead(m, i));
            right = gmfindtoken(slots, vexp, raw, cuthead(m, i+1));
            if (left<0 || right<0) {
                continue;
            }
            Arena tmp = scratch;
            Str   joined = concat(&tmp, takehead(m, i), cuthead(m, i+1));
            result = gmfindtoken(slots, vexp, raw, joined);
        }
        if (result < 0) {
            return 0;
        }
        u64 key = gmpairkey(left, right);
        i32 s   = gmslot(t->mergekey, t->mergeexp, key);
        if (!t->mergekey[s]) {  // the first, best ranked, wins
            t->mergekey[s]  = key;
            t->mergeto[s]   = result;
            t->mergerank[s] = rank;
        }
        made[result] = 1;
    }

    static Str const visible[] = {
        "<|channel>", "<channel|>", "<|tool_call>", "<tool_call|>",
        "<|tool_response>", "<tool_response|>", "<|\"|>",
    };
    t->text     = alloc<Str>(perm, GM_VOCAB);
    t->specials = alloc<GmSpecial>(perm, 256);
    t->pieceexp = 16;
    t->piecekey = alloc<u64>(perm, (iz)1<<t->pieceexp);
    t->pieceid  = alloc<i32>(perm, (iz)1<<t->pieceexp);
    iz npieces  = 0;
    for (i32 id = 0; id < GM_VOCAB; id++) {
        i32 type;
        copybytes(&type, types.at + (iz)id*4, 4);
        Str s = raw[id];
        b32 special = type == 3;
        for (i32 i = 0; i < countof(visible); i++) {
            special |= type==4 && s==visible[i];
        }
        if (special) {
            if (t->nspecials==256 || !s.len) {
                return 0;
            }
            t->specials[t->nspecials].text = clone(perm, s);
            t->specials[t->nspecials].id   = id;
            t->nspecials++;
            continue;
        }

        if (type == 6) {  // a byte, <0xNN>
            t->text[id] = gmhexbyte(perm, s);
        } else {
            Buf b(perm, s.len);
            for (iz i = 0; i < s.len; i++) {
                if (startswith(cuthead(s, i), "\xe2\x96\x81")) {
                    putbyte(&b, ' ');  // SentencePiece's space, U+2581
                    i += 2;
                } else {
                    putbyte(&b, s[i]);
                }
            }
            t->text[id] = finish(&b);
        }

        if (!made[id] && s.len>=1 && s.len<=4) {
            if (++npieces > ((iz)1<<t->pieceexp)/2) {
                return 0;
            }
            u64 key = gmpiecekey(s);
            i32 i   = gmslot(t->piecekey, t->pieceexp, key);
            if (!t->piecekey[i]) {
                t->piecekey[i] = key;
                t->pieceid[i]  = id;
            }
        }
    }
    sort(t->specials, t->nspecials, gmspeciallonger, scratch);
    return 1;
}

struct GmCand {
    i32 rank;
    i32 left;    // positions
    i32 right;
    i32 a;       // the tokens there when it was found
    i32 b;
    i32 result;
};

static b32 gmcandbefore(GmCand *a, GmCand *b)
{
    return a->rank!=b->rank ? a->rank<b->rank : a->left<b->left;
}

struct GmBpe {
    GmTok  *t;
    i32    *tokens;
    i32    *next;
    i32    *prev;
    GmCand *heap;
    i32     len;
};

static void gmbpepush(GmBpe *b, i32 left, i32 right)
{
    GmTok *t = b->t;
    u64    key = gmpairkey(b->tokens[left], b->tokens[right]);
    i32    s   = gmslot(t->mergekey, t->mergeexp, key);
    if (!t->mergekey[s]) {
        return;
    }
    i32 i = b->len++;
    GmCand c = {};
    c.rank   = t->mergerank[s];
    c.left   = left;
    c.right  = right;
    c.a      = b->tokens[left];
    c.b      = b->tokens[right];
    c.result = t->mergeto[s];
    b->heap[i] = c;
    for (; i && gmcandbefore(b->heap+i, b->heap+(i-1)/2); i = (i-1)/2) {
        GmCand swap = b->heap[i];
        b->heap[i] = b->heap[(i-1)/2];
        b->heap[(i-1)/2] = swap;
    }
}

static GmCand gmbpepop(GmBpe *b)
{
    GmCand top = b->heap[0];
    b->heap[0] = b->heap[--b->len];
    for (i32 i = 0;;) {
        i32 least = i, child = 2*i + 1;
        if (child < b->len && gmcandbefore(b->heap+child, b->heap+least)) {
            least = child;
        }
        if (child+1 < b->len && gmcandbefore(b->heap+child+1, b->heap+least)) {
            least = child + 1;
        }
        if (least == i) {
            break;
        }
        GmCand swap = b->heap[i];
        b->heap[i] = b->heap[least];
        b->heap[least] = swap;
        i = least;
    }
    return top;
}

// Apply the best-ranked merge, leftmost first, until none matches. The
// tokens form a linked list, and a heap holds candidate merges, those
// whose pair has since changed discarded. Returns the new count.
static i32 gmbpe(GmTok *t, i32 *tokens, i32 count, Arena scratch)
{
    if (count < 2) {
        return count;
    }
    GmBpe b = {};
    b.t      = t;
    b.tokens = tokens;
    b.next   = alloc<i32>(&scratch, count);
    b.prev   = alloc<i32>(&scratch, count);
    b.heap   = alloc<GmCand>(&scratch, 3*count);  // each merge adds two
    for (i32 i = 0; i < count; i++) {
        b.next[i] = i + 1;
        b.prev[i] = i - 1;
    }
    for (i32 i = 0; i+1 < count; i++) {
        gmbpepush(&b, i, i+1);
    }
    while (b.len) {
        GmCand c = gmbpepop(&b);
        if (b.next[c.left]!=c.right || tokens[c.left]!=c.a || tokens[c.right]!=c.b) {
            continue;  // stale
        }
        tokens[c.left]  = c.result;
        tokens[c.right] = -1;
        i32 after = b.next[c.left] = b.next[c.right];
        if (after < count) {
            b.prev[after] = c.left;
        }
        if (b.prev[c.left] >= 0) {
            gmbpepush(&b, b.prev[c.left], c.left);
        }
        if (after < count) {
            gmbpepush(&b, c.left, after);
        }
    }
    i32 out = 0;
    for (i32 i = 0; i < count; i = b.next[i]) {
        tokens[out++] = tokens[i];
    }
    return out;
}

// Encode text as tokens, no BOS. A special token is recognized only
// where allow, if given, is nonzero at its first byte: the template's
// own markup, never what a user wrote.
static Slice<i32> gmencode(GmTok *t, Arena *perm, Arena scratch, Str text, u8 *allow)
{
    i32 *ids = alloc<i32>(&scratch, text.len+1);
    i32  len = 0, run = 0;  // tokens, and where this BPE run began
    for (iz i = 0; i < text.len;) {
        i32 special = -1;
        if (allow && allow[i] && text[i]=='<') {
            Str rest = cuthead(text, i);
            for (i32 s = 0; s<t->nspecials && special<0; s++) {
                if (startswith(rest, t->specials[s].text)) {
                    special = s;
                }
            }
        }
        if (special >= 0) {
            len = run + gmbpe(t, ids+run, len-run, scratch);
            ids[len++] = t->specials[special].id;
            run = len;
            i += t->specials[special].text.len;
            continue;
        }

        Str piece = {};
        iz  used  = 1;
        if (text[i] == ' ') {
            piece = "\xe2\x96\x81";
        } else {
            iz n = 1;
            if ((text[i] & 0xc0) == 0xc0) {
                for (; n<4 && i+n<text.len && (text[i+n]&0xc0)==0x80; n++) {}
            }
            piece = {text.data+i, n};
            used  = n;
        }
        u64 key = gmpiecekey(piece);
        i32 s   = gmslot(t->piecekey, t->pieceexp, key);
        if (t->piecekey[s]) {
            ids[len++] = t->pieceid[s];
        } else {
            for (iz j = 0; j < piece.len; j++) {
                ids[len++] = 238 + piece[j];  // byte tokens, 238 to 493
            }
        }
        i += used;
    }
    len = run + gmbpe(t, ids+run, len-run, scratch);

    Slice<i32> r = {};
    r.data = alloc<i32>(perm, len);
    r.len  = r.cap = len;
    copybytes(r.data, ids, (iz)len*4);
    return r;
}


// Loading the GGUF file

static u64 ggread(u8 **at, u8 *end, iz size, b32 *bad)
{
    u64 v = 0;
    if (end-*at < size) {
        *bad = 1;
        return 0;
    }
    copybytes(&v, *at, size);  // little-endian, as is every host
    *at += size;
    return v;
}

static Str ggreadstr(u8 **at, u8 *end, b32 *bad)
{
    u64 len = ggread(at, end, 8, bad);
    if ((u64)(end-*at) < len) {
        *bad = 1;
        return {};
    }
    Str s = {*at, (iz)len};
    *at += len;
    return s;
}

static void ggskip(u8 **at, u8 *end, u32 type, i32 depth, b32 *bad)
{
    static i32 const sizes[16] = {1, 1, 2, 2, 4, 4, 4, 1, 0, 0, 8, 8, 8};
    if (type == 8) {
        ggreadstr(at, end, bad);
    } else if (type==9 && !depth) {
        u32 elem  = (u32)ggread(at, end, 4, bad);
        u64 count = ggread(at, end, 8, bad);
        if (elem<13 && elem!=8 && elem!=9) {
            if ((u64)(end-*at)/(u64)sizes[elem] < count) {
                *bad = 1;
            } else {
                *at += count*(u64)sizes[elem];
            }
        } else {
            for (u64 i = 0; i<count && !*bad; i++) {
                ggskip(at, end, elem, depth+1, bad);
            }
        }
    } else if (type<13 && type!=9) {
        ggread(at, end, sizes[type], bad);
    } else {
        *bad = 1;
    }
}

enum { GG_F32 = 0, GG_F16 = 1, GG_Q4_0 = 2, GG_Q6_K = 14 };

struct GgTensor {
    Str name;
    i64 dims[4];
    i32 ndims;
    u32 type;
    u8 *data;
};

struct GgIndex {
    GgTensor *list;
    i32       len;
    b32       bad;
};

// The tensor of that name, type and shape, rows by cols (cols 0 for a
// vector), or null, and the index marked bad unless optional and absent.
static u8 *ggbind(GgIndex *x, Str name, u32 type, i64 rows, i64 cols, b32 optional)
{
    for (i32 i = 0; i < x->len; i++) {
        GgTensor *t = x->list + i;
        if (t->name != name) {
            continue;
        }
        b32 ok = t->type==type && (cols ? t->ndims==2 && t->dims[0]==cols && t->dims[1]==rows
                                        : t->ndims==1 && t->dims[0]==rows);
        x->bad |= !ok;
        return ok ? t->data : 0;
    }
    x->bad |= !optional;
    return 0;
}

static GgTensor *ggfind(GgIndex *x, Str name)
{
    for (i32 i = 0; i < x->len; i++) {
        if (x->list[i].name == name) {
            return x->list + i;
        }
    }
    return 0;
}

static Str ggname(Arena *a, i32 layer, Str part)  // blk.N.PART.weight
{
    Buf b(a, 64);
    print(&b, "blk.");
    print(&b, (i64)layer);
    putbyte(&b, '.');
    print(&b, part);
    print(&b, ".weight");
    return finish(&b);
}

// A q4_0 matrix to bind, and where it goes.
struct GmQ4 {
    GmMat *m;
    u8    *src;
};

// Room for a q4_0 matrix in gmpackq4's layout.
static GmMat gmq4mat(Arena *perm, i32 rows, i32 cols)
{
    GmMat m = {};
    m.rows   = rows;
    m.cols   = cols;
    m.type   = GM_Q4;
    m.data   = allocbytes(perm, (iz)rows/8*(cols/32)*128);
    m.scales = alloc<u16>(perm, (iz)rows/8*(cols/32)*8);
    return m;
}

// Rearrange a group of 8 rows by 32 inputs of q4_0 (an f16 step, then
// 16 bytes, inputs 0-15 in the low nibbles and 16-31 in the high) into
// 8 chunks of 16 bytes: chunk c's byte j holds input c*4 + j%4 of row
// j/4 in its low nibble, and of row 4 + j/4 in its high.
static void gmpackq4(GmMat *m, u8 *src, iz g)
{
    iz blocks = m->cols/32;
    for (iz b = 0; b < blocks; b++) {
        u8 *out = m->data + (g*blocks + b)*128;
        for (i32 r = 0; r < 8; r++) {
            u8 *blk = src + ((g*8 + r)*blocks + b)*18;
            m->scales[(g*blocks + b)*8 + r] = (u16)(blk[0] | blk[1]<<8);
        }
        for (i32 c = 0; c < 8; c++) {
            for (i32 j = 0; j < 16; j++) {
                i32 in = c*4 + j%4;
                u8 *lo = src + ((g*8 + j/4)*blocks + b)*18 + 2;
                u8 *hi = src + ((g*8 + 4 + j/4)*blocks + b)*18 + 2;
                i32 l = in<16 ? lo[in] & 15 : lo[in-16] >> 4;
                i32 h = in<16 ? hi[in] & 15 : hi[in-16] >> 4;
                out[c*16 + j] = (u8)(l | h<<4);
            }
        }
    }
}

// One row of a matrix into the packed int8 layout: groups of 64 inputs
// with an f16 scale each, interleaved in blocks of 16 rows by 4 inputs.
static void gmpacki8row(GmMat *m, f32 *row, iz r)
{
    iz width = m->cols, groups = width/64;
    iz blk = r/16, in = r%16;
    i8 *values = (i8 *)m->data;
    for (iz g = 0; g < groups; g++) {
        f32 max = 0;
        for (i32 j = 0; j < 64; j++) {
            f32 a = gmabs(row[g*64+j]);
            max = a>max ? a : max;
        }
        f32 step = max/127.0f;
        u16 bits = gmtohalf(step>6.104e-05f ? step : 6.104e-05f);
        f32 scale = gmhalf(bits);  // quantized against the scale as stored
        m->scales[(blk*groups + g)*16 + in] = bits;
        for (i32 j = 0; j < 64; j++) {
            f32 q = gmrint(row[g*64+j] / scale);
            q = q>127 ? 127 : q<-127 ? -127 : q;
            values[blk*16*width + g*1024 + (j/4)*64 + in*4 + j%4] = (i8)q;
        }
    }
}

struct GmLoad {
    Gemma *g;
    GmQ4  *q4;
    i32    nq4;
    u8    *embedsrc;  // q6_K
    u8    *plmodelsrc;  // f16
    f32    theta[2];  // [full]
};

static void gmloadpass(void *arg, GmWork *w)
{
    GmLoad *l = (GmLoad *)arg;
    Gemma  *g = l->g;

    for (i32 i = 0; i < l->nq4; i++) {
        GmSpan s = gmspan(w, l->q4[i].m->rows/8);
        for (iz grp = s.beg; grp < s.end; grp++) {
            gmpackq4(l->q4[i].m, l->q4[i].src, grp);
        }
    }

    GmSpan s = gmspan(w, GM_VOCAB);
    for (iz r = s.beg; r < s.end; r++) {
        gmq6k(w->scratch, l->embedsrc + r*(GM_WIDTH/256*210), GM_WIDTH, 1);
        gmpacki8row(&g->embed, w->scratch, r);
    }
    s = gmspan(w, g->plmodel.rows);
    for (iz r = s.beg; r < s.end; r++) {
        u8 *src = l->plmodelsrc + r*GM_WIDTH*2;
        for (iz i = 0; i < GM_WIDTH; i++) {
            w->scratch[i] = gmhalf((u16)(src[2*i] | src[2*i+1]<<8));
        }
        gmpacki8row(&g->plmodel, w->scratch, r);
    }

    for (i32 full = 0; full < 2; full++) {
        i32 pairs = g->pairs[full];
        f32 hd    = full ? GM_FULLHD : GM_SLIDEHD;
        s = gmspan(w, g->ctx);
        for (iz p = s.beg; p < s.end; p++) {
            for (i32 j = 0; j < pairs; j++) {
                f32 freq  = 1 / __builtin_powf(l->theta[full], (f32)(2*j)/hd);
                f32 angle = (f32)p * freq;
                g->ropecos[full][p*pairs + j] = __builtin_cosf(angle);
                g->ropesin[full][p*pairs + j] = __builtin_sinf(angle);
            }
        }
    }
}

struct GmLoaded {
    Gemma *g;
    Str    err;  // when g is null
};

// Load the model from the mapped file, which must outlive it, for a
// context of ctx positions, its work done by the pool. Large: about
// 1.6 GB in perm, most of it the rearranged weights.
static GmLoaded gmload(GmPool *pool, Arena *perm, Arena scratch, Str file,
                       i32 ctx, i32 isa)
{
    GmLoaded r = {};
    b32      bad = 0;
    u8      *at  = file.data;
    u8      *end = file.data + file.len;
    if (!startswith(file, "GGUF")) {
        r.err = "not a GGUF file";
        return r;
    }
    at += 4;
    u32 version = (u32)ggread(&at, end, 4, &bad);
    u64 ntensors = ggread(&at, end, 8, &bad);
    u64 nmeta    = ggread(&at, end, 8, &bad);
    if (bad || version!=3 || ntensors>4096) {
        r.err = "unsupported GGUF file";
        return r;
    }

    // The metadata it needs, checked against the architecture built in
    GgArray tokens = {}, types = {}, merges = {}, pattern = {};
    u64 align = 32;
    i64 layers = 0, width = 0, window = 0, context = 0, perlayer = 0, shared = 0;
    f32 theta = 0, thetaslide = 0;
    b32 gemma4 = 0;
    for (u64 i = 0; i<nmeta && !bad; i++) {
        Str key   = ggreadstr(&at, end, &bad);
        u32 type  = (u32)ggread(&at, end, 4, &bad);
        u8 *value = at;
        if (type == 9) {
            GgArray a = {};
            a.type  = (u32)ggread(&at, end, 4, &bad);
            a.count = ggread(&at, end, 8, &bad);
            a.at    = at;
            a.end   = end;
            if (key == "tokenizer.ggml.tokens") {
                tokens = a;
            } else if (key == "tokenizer.ggml.token_type") {
                types = a;
            } else if (key == "tokenizer.ggml.merges") {
                merges = a;
            } else if (key == "gemma4.attention.sliding_window_pattern") {
                pattern = a;
            }
            at = value;
        }
        ggskip(&at, end, type, 0, &bad);
        if (bad || type==9) {
            continue;
        }
        u32 u = 0;
        f32 f = 0;
        if (type==4 || type==5) copybytes(&u, value, 4);
        if (type == 6) copybytes(&f, value, 4);
        if (key == "general.architecture") {
            gemma4 = type==8 && span(value+8, at)=="gemma4";
        } else if (key == "general.alignment") {
            align = u;
        } else if (key == "gemma4.block_count") {
            layers = u;
        } else if (key == "gemma4.embedding_length") {
            width = u;
        } else if (key == "gemma4.attention.sliding_window") {
            window = u;
        } else if (key == "gemma4.context_length") {
            context = u;
        } else if (key == "gemma4.embedding_length_per_layer_input") {
            perlayer = u;
        } else if (key == "gemma4.attention.shared_kv_layers") {
            shared = u;
        } else if (key == "gemma4.rope.freq_base") {
            theta = f;
        } else if (key == "gemma4.rope.freq_base_swa") {
            thetaslide = f;
        }
    }
    b32 patternok = pattern.type==7 && pattern.count==GM_LAYERS &&
                    pattern.end-pattern.at >= GM_LAYERS;
    for (i32 i = 0; patternok && i < GM_LAYERS; i++) {
        patternok = pattern.at[i] == (i%GM_EVERY != GM_EVERY-1);  // sliding
    }
    if (bad || !gemma4 || layers!=GM_LAYERS || width!=GM_WIDTH ||
        window!=GM_WINDOW || context!=GM_MAXCTX || perlayer!=GM_PERLAYER ||
        shared!=GM_LAYERS-GM_OWNKV || !patternok || !(theta>0) ||
        !(thetaslide>0) || !align || align&(align-1) || align>4096) {
        r.err = "not the Gemma 4 E2B model this program runs";
        return r;
    }

    GgIndex x = {};
    x.list = alloc<GgTensor>(&scratch, (iz)ntensors);
    x.len  = (i32)ntensors;
    for (i32 i = 0; i<x.len && !bad; i++) {
        GgTensor *t = x.list + i;
        t->name  = ggreadstr(&at, end, &bad);
        t->ndims = (i32)ggread(&at, end, 4, &bad);
        if (t->ndims<1 || t->ndims>4) {
            bad = 1;
            break;
        }
        for (i32 d = 0; d < t->ndims; d++) {
            u64 dim = ggread(&at, end, 8, &bad);
            bad |= dim<1 || dim>((u64)1<<30);
            t->dims[d] = (i64)dim;
        }
        t->type = (u32)ggread(&at, end, 4, &bad);
        u64 off = ggread(&at, end, 8, &bad);
        t->data = (u8 *)(uz)off;  // an offset, for now
    }
    u64 start = ((u64)(at - file.data) + align - 1) & ~(align - 1);
    for (i32 i = 0; i<x.len && !bad; i++) {
        GgTensor *t = x.list + i;
        u64 count = 1;
        for (i32 d = 0; d<t->ndims && !bad; d++) {
            bad |= count > ((u64)1<<40)/(u64)t->dims[d];
            count *= (u64)t->dims[d];
        }
        u64 bytes = t->type==GG_F32  ? count*4 :
                    t->type==GG_F16  ? count*2 :
                    t->type==GG_Q4_0 ? count/32*18 :
                    t->type==GG_Q6_K ? count/256*210 : 0;
        u64 off   = (u64)(uz)t->data;
        u64 size  = (u64)file.len;
        bad |= off>size || start>size-off || bytes>size-start-off || (start+off)%4;
        t->data = file.data + start + off;
    }
    if (bad) {
        r.err = "truncated or malformed GGUF file";
        return r;
    }

    Gemma *g = alloc<Gemma>(perm);
    g->pool = pool;
    g->isa  = isa;
    g->k    = gmkern(isa);
    g->ctx  = ctx<1 ? 1 : ctx>GM_MAXCTX ? GM_MAXCTX : ctx;
    if (!gmbuildtok(&g->tok, perm, scratch, tokens, types, merges)) {
        r.err = "unexpected tokenizer";
        return r;
    }

    // Bind every tensor, checking its shape
    u8 *embedsrc = ggbind(&x, "token_embd.weight", GG_Q6_K, GM_VOCAB, GM_WIDTH, 0);
    g->plembed = ggbind(&x, "per_layer_token_embd.weight", GG_Q6_K, GM_VOCAB,
                        GM_LAYERS*GM_PERLAYER, 0);
    g->norm    = (f32 *)ggbind(&x, "output_norm.weight", GG_F32, GM_WIDTH, 0, 0);
    u8 *plmodelsrc = ggbind(&x, "per_layer_model_proj.weight", GG_F16,
                            GM_LAYERS*GM_PERLAYER, GM_WIDTH, 0);
    g->plnorm  = (f32 *)ggbind(&x, "per_layer_proj_norm.weight", GG_F32, GM_PERLAYER, 0, 0);
    f32 *ropefreqs = (f32 *)ggbind(&x, "rope_freqs.weight", GG_F32, GM_FULLHD/2, 0, 0);

    GmQ4 *q4  = alloc<GmQ4>(&scratch, GM_LAYERS*9);
    i32   nq4 = 0;
    for (i32 i = 0; i<GM_LAYERS && !x.bad; i++) {
        GmLayer *l = g->layers + i;
        l->full = i%GM_EVERY == GM_EVERY-1;
        l->hd   = l->full ? GM_FULLHD : GM_SLIDEHD;
        b32 own = i < GM_OWNKV;
        i32 qw  = GM_HEADS*l->hd;

        Arena t = scratch;
        l->attnnorm = (f32 *)ggbind(&x, ggname(&t, i, "attn_norm"), GG_F32, GM_WIDTH, 0, 0);
        l->postattn = (f32 *)ggbind(&x, ggname(&t, i, "post_attention_norm"), GG_F32, GM_WIDTH, 0, 0);
        l->ffnnorm  = (f32 *)ggbind(&x, ggname(&t, i, "ffn_norm"), GG_F32, GM_WIDTH, 0, 0);
        l->postffn  = (f32 *)ggbind(&x, ggname(&t, i, "post_ffw_norm"), GG_F32, GM_WIDTH, 0, 0);
        l->postpl   = (f32 *)ggbind(&x, ggname(&t, i, "post_norm"), GG_F32, GM_WIDTH, 0, 0);
        l->qnorm    = (f32 *)ggbind(&x, ggname(&t, i, "attn_q_norm"), GG_F32, l->hd, 0, 0);
        l->knorm    = (f32 *)ggbind(&x, ggname(&t, i, "attn_k_norm"), GG_F32, l->hd, 0, !own);
        f32 *scalar = (f32 *)ggbind(&x, ggname(&t, i, "layer_output_scale"), GG_F32, 1, 0, 0);
        l->scalar   = scalar ? *scalar : 0;

        GgTensor *gate = ggfind(&x, ggname(&t, i, "ffn_gate"));
        i64 ffn = gate && gate->ndims==2 ? gate->dims[1] : 0;
        if (ffn<64 || ffn>GM_FFNMAX || ffn%64) {
            x.bad = 1;
            break;
        }
        struct {
            GmMat *m;
            Str    part;
            i64    rows;
            i64    cols;
            b32    optional;
        } mats[] = {
            {&l->q,      "attn_q",      qw,          GM_WIDTH,    0},
            {&l->k,      "attn_k",      l->hd,       GM_WIDTH,    !own},
            {&l->v,      "attn_v",      l->hd,       GM_WIDTH,    !own},
            {&l->o,      "attn_output", GM_WIDTH,    qw,          0},
            {&l->gate,   "ffn_gate",    ffn,         GM_WIDTH,    0},
            {&l->up,     "ffn_up",      ffn,         GM_WIDTH,    0},
            {&l->down,   "ffn_down",    GM_WIDTH,    ffn,         0},
            {&l->plgate, "inp_gate",    GM_PERLAYER, GM_WIDTH,    0},
            {&l->plproj, "proj",        GM_WIDTH,    GM_PERLAYER, 0},
        };
        for (i32 j = 0; j < countof(mats); j++) {
            u8 *src = ggbind(&x, ggname(&t, i, mats[j].part), GG_Q4_0, mats[j].rows,
                             mats[j].cols, mats[j].optional);
            if (src) {
                *mats[j].m = gmq4mat(perm, (i32)mats[j].rows, (i32)mats[j].cols);
                q4[nq4++] = {mats[j].m, src};
            }
        }
        x.bad |= !own && (l->k.data || l->knorm);
        x.bad |= own && !l->k.data;
    }
    if (x.bad) {
        r.err = "not the Gemma 4 E2B model this program runs";
        return r;
    }

    // Proportional RoPE: the full layers rotate only the pairs whose
    // factor is 1 (the rest are 1e30, unrotated), at frequencies set by
    // the whole head.
    g->pairs[0] = GM_SLIDEHD/2;
    for (; g->pairs[1]<GM_FULLHD/2 && ropefreqs[g->pairs[1]]==1.0f; g->pairs[1]++) {}
    for (i32 full = 0; full < 2; full++) {
        g->ropecos[full] = (f32 *)allocbytes(perm, (iz)g->ctx*g->pairs[full]*4 + 64);
        g->ropesin[full] = (f32 *)allocbytes(perm, (iz)g->ctx*g->pairs[full]*4 + 64);
    }

    g->gelu = alloc<f32>(perm, GM_GELUN);
    for (i32 i = 0; i < GM_GELUN; i++) {
        f32 v = -8.0f + 16.0f*(f32)i/(GM_GELUN - 1);
        g->gelu[i] = 0.5f*v*(1.0f + __builtin_tanhf(0.7978845608f*(v + 0.044715f*v*v*v)));
    }

    g->embed.rows   = GM_VOCAB;
    g->embed.cols   = GM_WIDTH;
    g->embed.type   = GM_I8;
    g->embed.data   = (u8 *)alloc<u64>(perm, (iz)GM_VOCAB*GM_WIDTH/8);
    g->embed.scales = alloc<u16>(perm, (iz)GM_VOCAB*GM_WIDTH/64);
    g->plmodel.rows   = GM_LAYERS*GM_PERLAYER;
    g->plmodel.cols   = GM_WIDTH;
    g->plmodel.type   = GM_I8;
    g->plmodel.data   = (u8 *)alloc<u64>(perm, (iz)g->plmodel.rows*GM_WIDTH/8);
    g->plmodel.scales = alloc<u16>(perm, (iz)g->plmodel.rows*GM_WIDTH/64);

    GmLoad l = {};
    l.g          = g;
    l.q4         = q4;
    l.nq4        = nq4;
    l.embedsrc   = embedsrc;
    l.plmodelsrc = plmodelsrc;
    l.theta[0]   = thetaslide;
    l.theta[1]   = theta;
    gmrun(pool, gmloadpass, &l);

    iz big = (iz)GM_BATCH*GM_FFNMAX;
    g->residual = alloc<f32>(perm, (iz)GM_BATCH*GM_WIDTH);
    g->hidden   = alloc<f32>(perm, big);
    g->aux      = alloc<f32>(perm, big);
    g->quant    = (i8 *)alloc<u64>(perm, big/8);
    g->qscale   = alloc<f32>(perm, big/64);
    g->plin     = alloc<f32>(perm, (iz)GM_BATCH*GM_LAYERS*GM_PERLAYER);
    g->logits   = alloc<f32>(perm, GM_VOCAB);
    r.g = g;
    return r;
}


// The developers' commands, to check the port against gemma4.c:
//   prospero builtin-bench MODEL [PREFILL [GENERATE]]
//   prospero builtin-logits MODEL TEXT  (every position's, f32, on stdout)

#if PROSPERO_DEVTOOLS
// Load the model for a dev command, telling how on stderr, or null.
static Gemma *gmdevload(Os *os, Arena *perm, Arena scratch, Str path, i32 ctx)
{
    Arena *model = alloc<Arena>(perm);
    *model = newarena(os_reserve(os, (iz)4<<30));
    Str file = os_guimap(os, scratch, path);
    if (!file.len || !model->beg) {
        os_print(os, 2, "prospero: could not map the model\n");
        return 0;
    }

    i32 threads = gmthreads(os_guicores(os, scratch));
    Str env = os_getenv(os, &scratch, "PROSPERO_BUILTIN_THREADS");
    if (env.len) {
        Int n = parseint(env);
        threads = n.ok && n.val>0 ? (i32)n.val : threads;
    }
    i32 isa = gmisa(os_guiarmdot(os));
    i64 t0  = os_ticks(os);
    os_guithreadfast(os);
    GmPool  *pool = gmpool(os, perm, threads, GM_SCRATCH,
                           os_guithreadfast);
    GmLoaded m    = gmload(pool, model, scratch, file, ctx, isa);
    Buf b(&scratch);
    if (!m.g) {
        print(&b, "prospero: ");
        print(&b, m.err);
        print(&b, "\n");
        os_print(os, 2, finish(&b));
        return 0;
    }
    print(&b, "loaded in ");
    print(&b, (os_ticks(os) - t0)/1000);
    print(&b, " ms, ");
    print(&b, (i64)pool->n);
    print(&b, isa==GM_AVX2 ? Str(" threads, AVX2\n") :
              isa==GM_NEONDOT ? Str(" threads, NEON dot\n") : Str(" threads, scalar\n"));
    os_print(os, 2, finish(&b));
    return m.g;
}

static i32 gmmain(Os *os, Arena perm, Str *args, i32 nargs)
{
    Arena scratch = newarena(os_reserve(os, (iz)1<<30));
    b32   bench   = args[1] == "builtin-bench";
    if (nargs<3 || (!bench && nargs!=4) || !scratch.beg) {
        os_print(os, 2, "usage: prospero builtin-bench MODEL [PREFILL [GENERATE]]\n"
                        "       prospero builtin-logits MODEL TEXT\n");
        return 1;
    }
    i32    ctx = 65536;  // as the window lists (GM_CONTEXT)
    Gemma *g   = gmdevload(os, &perm, scratch, args[2], ctx);
    if (!g) {
        return 1;
    }
    GmKv kv = gmkvnew(os, ctx);

    if (!bench) {
        Slice<i32> enc = gmencode(&g->tok, &perm, scratch, args[3], 0);
        i32 *tokens = alloc<i32>(&scratch, enc.len+1);
        tokens[0] = GM_BOS;
        copybytes(tokens+1, enc.data, enc.len*4);
        i32 n = (i32)enc.len + 1;
        for (i32 done = 0; done < n;) {
            i32 chunk = GM_BATCH - done%GM_BATCH;
            chunk = chunk<n-done ? chunk : n-done;
            gmforward(g, &kv, tokens+done, chunk, done);
            for (i32 i = 0; i < chunk; i++) {
                f32 *v = gmlogits(g, i);
                os_print(os, 1, {(u8 *)v, (iz)GM_VOCAB*4});
            }
            done += chunk;
        }
        return 0;
    }

    i32 pp = 512, tg = 128;
    if (nargs > 3) {
        Int n = parseint(args[3]);
        pp = n.ok && n.val>=0 && n.val<ctx ? (i32)n.val : pp;
    }
    if (nargs > 4) {
        Int n = parseint(args[4]);
        tg = n.ok && n.val>=0 ? (i32)(n.val<ctx ? n.val : ctx) : tg;
    }
    tg = tg<ctx-pp ? tg : ctx-pp;
    Buf b(&scratch);
    if (pp) {
        i32 *tokens = alloc<i32>(&scratch, pp);
        for (i32 i = 0; i < pp; i++) {
            tokens[i] = 2 + i%1000;
        }
        i64 start = os_ticks(os);
        for (i32 done = 0; done < pp;) {
            i32 chunk = GM_BATCH - done%GM_BATCH;
            chunk = chunk<pp-done ? chunk : pp-done;
            gmforward(g, &kv, tokens+done, chunk, done);
            done += chunk;
        }
        f64 secs = (f64)(os_ticks(os) - start) / 1e6;
        print(&b, "pp");
        print(&b, (i64)pp);
        print(&b, " ");
        printfloat(&b, pp/secs, 2);
        print(&b, " tok/s\n");
    }
    if (tg) {
        i32 token = GM_BOS;
        i64 start = os_ticks(os);
        for (i32 pos = 0; pos < tg; pos++) {
            gmforward(g, &kv, &token, 1, pp+pos);  // after the prefill
            token = gmargmax(gmlogits(g, 0), GM_VOCAB);
        }
        f64 secs = (f64)(os_ticks(os) - start) / 1e6;
        print(&b, "tg");
        print(&b, (i64)tg);
        print(&b, " ");
        printfloat(&b, tg/secs, 2);
        print(&b, " tok/s\n");
    }
    os_print(os, 1, finish(&b));
    return 0;
}
#endif


// This file is derived from gemma4.c, under its license:
//
// MIT License
//
// Copyright (c) 2026 Qmog AI
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
