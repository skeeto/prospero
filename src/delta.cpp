// Deltas: a string made of copies from a base and literal additions
//
// A relayed request is usually the last one with a little changed: a
// message appended, a cache marker moved (anthropic.cpp). So the thin
// engine sends the ops that make the new body from the last one it sent
// the same client, which keeps it (relay.cpp). The ops are varints: a
// tag v, then for v odd a copy of v>>1 bytes from the base at the varint
// offset that follows, else an addition of the v>>1 literal bytes that
// follow. No op is empty.
//
// Matches: the common prefix and suffix first, then the base's aligned
// 32-byte blocks, hashed, looked up at every position of the rest by a
// rolling hash and extended both ways. That finds any shared run of 63
// bytes or more, wherever it moved.
//
// This is free and unencumbered software released into the public domain.

enum : iz {
    DELTA_BLOCK = 32,
};

static u64 const deltamul = 0x100000001b3u;  // the rolling hash's base

static void deltaputv(Buf *b, u64 v)
{
    do {
        u8 c = (u8)(v & 0x7f);
        v >>= 7;
        putbyte(b, (u8)(c | (v ? 0x80 : 0)));
    } while (v);
}

// A varint at *at, which moves past it. 0 if cut short or too long.
static b32 deltagetv(Str s, iz *at, u64 *v)
{
    u64 r = 0;
    for (i32 shift = 0; shift < 63; shift += 7) {
        if (*at >= s.len) {
            return 0;
        }
        u8 c = s.data[(*at)++];
        r |= (u64)(c & 0x7f) << shift;
        if (!(c & 0x80)) {
            *v = r;
            return 1;
        }
    }
    return 0;
}

static void deltaadd(Buf *b, u8 *p, iz len)
{
    if (len) {
        deltaputv(b, (u64)len<<1);
        print(b, Str(p, len));
    }
}

static void deltacopy(Buf *b, iz off, iz len)
{
    if (len) {
        deltaputv(b, (u64)len<<1 | 1);
        deltaputv(b, (u64)off);
    }
}

static u64 deltahash(u8 *p)
{
    u64 h = 0;
    for (iz i = 0; i < DELTA_BLOCK; i++) {
        h = h*deltamul + p[i];
    }
    return h;
}

struct DeltaSlot {
    u64 hash;
    iz  off;   // +1, 0 if empty
};

// The ops that make next from base, in a, temporaries in scratch, which
// needs about base.len/2 bytes.
static Str deltamake(Arena *a, Arena scratch, Str base, Str next)
{
    u8 *bp  = base.data;
    u8 *np  = next.data;
    iz  lim = base.len<next.len ? base.len : next.len;
    iz  pre = 0;
    for (; pre<lim && bp[pre]==np[pre]; pre++) {}
    iz  suf = 0;
    for (; suf<lim-pre && bp[base.len-1-suf]==np[next.len-1-suf]; suf++) {}

    // The base's blocks by hash, the first of equal ones
    iz nb  = base.len / DELTA_BLOCK;
    iz cap = 1;
    while (cap < 2*nb) {
        cap *= 2;
    }
    DeltaSlot *tab = alloc<DeltaSlot>(&scratch, cap);
    for (iz k = 0; k < nb; k++) {
        u64 h = deltahash(bp + k*DELTA_BLOCK);
        iz  i = (iz)(h & (u64)(cap-1));
        for (; tab[i].off && tab[i].hash!=h; i = (i+1) & (cap-1)) {}
        if (!tab[i].off) {
            tab[i].hash = h;
            tab[i].off  = k*DELTA_BLOCK + 1;
        }
    }
    u64 top = 1;  // deltamul to the power of DELTA_BLOCK-1
    for (iz i = 1; i < DELTA_BLOCK; i++) {
        top *= deltamul;
    }

    Buf out(a, 64 + (next.len-pre-suf)/4);
    deltacopy(&out, 0, pre);
    iz  end  = next.len - suf;
    iz  lit  = pre;  // where the literals waiting to go start
    iz  i    = pre;
    u64 h    = 0;
    b32 have = 0;
    while (nb && i+DELTA_BLOCK<=end) {
        if (!have) {
            h    = deltahash(np + i);
            have = 1;
        }
        iz s = (iz)(h & (u64)(cap-1));
        for (; tab[s].off && tab[s].hash!=h; s = (s+1) & (cap-1)) {}
        iz off = tab[s].off - 1;
        if (off>=0 && !__builtin_memcmp(bp+off, np+i, DELTA_BLOCK)) {
            iz bo = off;
            iz no = i;
            for (; no>lit && bo>0 && bp[bo-1]==np[no-1]; bo--, no--) {}
            iz be = off + DELTA_BLOCK;
            iz ne = i + DELTA_BLOCK;
            for (; ne<end && be<base.len && bp[be]==np[ne]; be++, ne++) {}
            deltaadd(&out, np+lit, no-lit);
            deltacopy(&out, bo, ne-no);
            i    = lit = ne;
            have = 0;
            continue;
        }
        if (i+DELTA_BLOCK < end) {
            h = (h - np[i]*top)*deltamul + np[i+DELTA_BLOCK];
        }
        i++;
    }
    deltaadd(&out, np+lit, end-lit);
    deltacopy(&out, base.len-suf, suf);
    return finish(&out);
}

// The n bytes the ops make from base, in a, else null where the ops are
// unsound: cut short, reaching past the base, or making other than n
// bytes. The caller bounds n.
static Str deltaapply(Arena *a, Str base, Str ops, iz n)
{
    u8 *out = alloc<u8>(a, n);
    iz  len = 0;
    for (iz at = 0; at < ops.len;) {
        u64 v = 0;
        if (!deltagetv(ops, &at, &v)) {
            return {};
        }
        u64 m = v >> 1;
        if (!m || m>(u64)(n-len)) {
            return {};
        }
        if (v & 1) {
            u64 off = 0;
            if (!deltagetv(ops, &at, &off) || off>(u64)base.len ||
                    m>(u64)base.len-off) {
                return {};
            }
            copybytes(out+len, base.data+off, (iz)m);
        } else {
            if (m > (u64)(ops.len-at)) {
                return {};
            }
            copybytes(out+len, ops.data+at, (iz)m);
            at += (iz)m;
        }
        len += (iz)m;
    }
    return len==n ? Str(out, n) : Str{};
}
