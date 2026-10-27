// Arena aliasing checks, for tests and Debug builds (PROSPERO_ARENACHECK)
//
// Two live arenas sharing a frontier is the arena's characteristic bug:
// f(&a, a), whose scratch starts where its results go, or a copy of a
// frame that allocates over what another copy still reads. Each arena
// that allocates gets an identity, whose claim is [lo, beg): the bytes
// it allocated since it came to be. An allocation over another live
// identity's claim is noted on that identity, as an overlap, but is no
// fault yet: many copies are safe only by ordering, done with before
// the original allocates over them. It is a fault when the identity
// overlapped allocates again while its frontier is still above those
// bytes, which it treats as its own. Allocating through an arena whose
// object was destroyed, as through a Buf bound to a dead local, is a
// fault too.
//
// Bytes overwritten and then only read go unseen: a record parsed in a
// copy of scratch, then scratch given by value to a callee before the
// parse is read (f9108f7). Faulting on a copy allocating over a live
// copy of the same arena would catch it, but the arguments of a call
// live to the end of the full expression, so f(scratch) || g(scratch)
// looks the same.
//
// The checker never reads another Arena object: identities mirror
// their frontier, which an arena moved down by hand (a trim) leaves
// stale until it next allocates. Which identity last allocated a 16-byte
// granule is kept in a direct-mapped table of hints, lossy by design: a
// hint is only checked against its identity's claim as it stands, so a
// lost or aliased hint can lose a finding but never invent one. So can
// running out of identities, as arenas abandoned in memory that is
// reused never give theirs back.
//
// A fault is reported through os_arenafault and traps, unless tests
// set arenacheck.record to count them instead.

enum : u32 {
    ARENA_SLOTS = 1u<<15,
    ARENA_HINTS = 1u<<20,
    ARENA_DEAD  = 0xffffffffu,  // a destroyed arena's aid
    ARENA_SPAN  = 1u<<12,       // granules checked in full per allocation
};

// Another arena allocated over [lo, hi) of this one's claim.
struct ArenaOver {
    byte       *lo;
    byte       *hi;
    char const *file;  // that arena's creation site
    i32         line;
    void       *pc;    // its allocation
};

struct ArenaRec {
    u32          aid;   // 0 while free
    u32          gen;
    Arena const *self;
    byte        *lo;
    byte        *beg;
    char const  *file;
    i32          line;
    ArenaOver    over;  // the first overlap noted, if any (over.hi)
};

// Provided by the platform layer: write a fault's report to stderr.
static void os_arenafault(Str);

// Shared by every translation unit of a program that includes base.cpp,
// as Windows' does with its desktop helper (inline linkage), since a
// struct with an Arena in it copies through either's code.
struct ArenaCheck {
    i32      lock;
    b32      record;   // tests: count faults instead of trapping
    i32      faults;
    u8       last[512];
    iz       lastlen;
    u32      used;     // slots ever used
    u32      nfree;
    u32      evict;
    u32      free[ARENA_SLOTS];
    u32      hints[ARENA_HINTS];
    ArenaRec recs[ARENA_SLOTS];
};
inline ArenaCheck arenacheck;

inline void arenalock()
{
    while (__atomic_exchange_n(&arenacheck.lock, 1, __ATOMIC_ACQUIRE)) {}
}

inline void arenaunlock()
{
    __atomic_store_n(&arenacheck.lock, 0, __ATOMIC_RELEASE);
}

inline ArenaRec *arenarec(u32 aid)
{
    u32 slot = (aid & 0xffff) - 1;
    if (!aid || aid==ARENA_DEAD || slot>=ARENA_SLOTS) {
        return 0;
    }
    ArenaRec *r = arenacheck.recs + slot;
    return r->aid==aid ? r : 0;
}

// The identity registered by this very object, if it still holds one.
inline ArenaRec *arenaown(Arena const *a)
{
    ArenaRec *r = arenarec(a->aid);
    return r && r->self==a ? r : 0;
}

inline ArenaRec *arenanew(Arena const *a, byte *lo)
{
    u32 slot = 0;
    if (arenacheck.nfree) {
        slot = arenacheck.free[--arenacheck.nfree];
    } else if (arenacheck.used < ARENA_SLOTS) {
        slot = arenacheck.used++;
    } else {
        // Full of identities never given back: the evicted one's arena
        // registers anew when it next allocates
        slot = arenacheck.evict++ % ARENA_SLOTS;
    }
    ArenaRec *r = arenacheck.recs + slot;
    u32 gen = (r->gen + 1) & 0xffff;
    *r = {};
    r->gen  = gen;
    r->aid  = gen<<16 | (slot + 1);
    r->self = a;
    r->lo   = lo;
    r->beg  = lo;
    r->file = a->file;
    r->line = a->line;
    a->aid  = r->aid;
    return r;
}

inline void arenafree(ArenaRec *r)
{
    r->aid  = 0;
    r->self = 0;
    arenacheck.free[arenacheck.nfree++] = (u32)(r - arenacheck.recs);
}

inline u32 *arenahint(byte *p)
{
    return arenacheck.hints + ((uz)p>>4 & (ARENA_HINTS - 1));
}

// Calls f on the granules of [lo, hi): all of them in a short range,
// else a sample spread over it, both ends included.
template<typename F>
inline void arenagranules(byte *lo, byte *hi, F f)
{
    if (lo >= hi) {
        return;
    }
    uz first = (uz)lo >> 4;
    uz last  = (uz)(hi - 1) >> 4;
    uz step  = (last - first)/ARENA_SPAN + 1;
    for (uz g = first; g <= last; g += step) {
        f((byte *)(g << 4));
    }
    if ((last - first) % step) {
        f((byte *)(last << 4));
    }
}


// Reports

struct ArenaMsg {
    u8 *buf;
    iz  len;
    iz  cap;
};

inline void arenaput(ArenaMsg *m, Str s)
{
    for (iz i = 0; i<s.len && m->len<m->cap; i++) {
        m->buf[m->len++] = s.data[i];
    }
}

inline void arenaputs(ArenaMsg *m, char const *s)
{
    Str r = {};
    r.cdata = s ? s : "?";
    while (r.cdata[r.len]) r.len++;
    arenaput(m, r);
}

inline void arenaputn(ArenaMsg *m, uz v, i32 base)
{
    u8  tmp[24];
    u8 *end = tmp + countof(tmp);
    u8 *beg = end;
    do {
        *--beg = (u8)"0123456789abcdef"[v % (uz)base];
    } while (v /= (uz)base);
    if (base == 16) {
        arenaput(m, "0x");
    }
    arenaput(m, {beg, end - beg});
}

inline void arenaputsite(ArenaMsg *m, char const *file, i32 line)
{
    arenaputs(m, file);
    arenaput(m, ":");
    arenaputn(m, (uz)line, 10);
}

// Called without the lock held.
inline void arenafault(u8 *buf, iz len)
{
    Str msg = {buf, len};
    if (arenacheck.record) {
        arenalock();
        arenacheck.faults++;
        copybytes(arenacheck.last, buf, len);
        arenacheck.lastlen = len;
        arenaunlock();
        return;
    }
    os_arenafault(msg);
    __builtin_trap();
}


// Hooks

// Before a moves its frontier past [p, p+n), from a->beg.
inline void arenacheck_alloc(Arena *a, byte *p, iz n, void *pc)
{
    u8       buf[sizeof(arenacheck.last)];
    ArenaMsg m = {buf, 0, countof(buf)};

    if (a->aid == ARENA_DEAD) {
        arenaput(&m, "arena fault: an allocation through an arena "
                     "already destroyed, made at ");
        arenaputsite(&m, a->file, a->line);
        arenaput(&m, " (allocation at ");
        arenaputn(&m, (uz)pc, 16);
        arenaput(&m, ")\n");
        arenafault(m.buf, m.len);
        return;
    }

    arenalock();
    byte     *old = a->beg;
    byte     *hi  = p + n;
    ArenaRec *r   = arenaown(a);
    r = r ? r : arenanew(a, old);

    ArenaOver o = r->over;
    if (o.hi && o.lo>=r->lo && o.lo<old) {
        arenaput(&m, "arena fault: the arena made at ");
        arenaputsite(&m, r->file, r->line);
        arenaput(&m, " allocated again while holding [");
        arenaputn(&m, (uz)o.lo, 16);
        arenaput(&m, ", ");
        arenaputn(&m, (uz)o.hi, 16);
        arenaput(&m, "), which the arena made at ");
        arenaputsite(&m, o.file, o.line);
        arenaput(&m, " allocated over (allocation at ");
        arenaputn(&m, (uz)o.pc, 16);
        arenaput(&m, ")\n");
    }
    r->over = {};
    r->lo   = old<r->lo ? old : r->lo;
    r->beg  = hi;

    arenagranules(p, hi, [&](byte *g) {
        u32      *h = arenahint(g);
        ArenaRec *v = *h!=r->aid ? arenarec(*h) : 0;
        if (v && !v->over.hi) {
            byte *lo  = p>v->lo ? p : v->lo;
            byte *top = hi<v->beg ? hi : v->beg;
            b32   lent = a->lender==v->aid && lo>=a->lentlo && top<=a->lenthi;
            if (lo<top && !lent) {
                v->over = {lo, top, r->file, r->line, pc};
            }
        }
        *h = r->aid;
    });
    arenaunlock();

    if (m.len) {
        arenafault(m.buf, m.len);
    }
}

// A copy's parent is the arena copied, which registers for it, so that a
// copy given back to it is known as one (operator=).
inline Arena::Arena(Arena const &a, char const *file, i32 line)
    : beg{a.beg}, end{a.end}, region{a.region},
      lender{a.lender}, lentlo{a.lentlo}, lenthi{a.lenthi},
      file{file}, line{line}
{
    if (a.aid == ARENA_DEAD) {
        parent = a.parent;
        return;
    }
    arenalock();
    ArenaRec *r = arenaown(&a);
    r = r ? r : arenanew(&a, a.beg);
    parent = r->aid;
    arenaunlock();
}

inline Arena &Arena::operator=(Arena const &a)
{
    if (this == &a) {
        return *this;
    }
    if (aid || a.aid) {
        arenalock();
        ArenaRec *d = arenaown(this);
        ArenaRec *s = arenaown(&a);
        if (d && a.parent==aid) {
            // A copy of this one given back: a commit or a restore. This
            // claim now ends where the copy's did, and the copy's bytes
            // are this one's.
            d->beg = a.beg;
            d->lo  = a.beg<d->lo ? a.beg : d->lo;
            if (s) {
                u32 from = s->aid;
                arenagranules(s->lo, s->beg, [&](byte *g) {
                    u32 *h = arenahint(g);
                    *h = *h==from ? d->aid : *h;
                });
                d->over = d->over.hi ? d->over : s->over;
                s->lo   = s->beg = a.beg;
                s->over = {};
            }
        } else if (!d || beg<=d->lo) {
            // This one holds nothing: a hand-off, which takes the other's
            // identity, claim and all
            if (d) {
                arenafree(d);
            }
            aid = 0;
            if (s) {
                aid     = s->aid;
                s->self = this;
                a.aid   = 0;
            }
            parent = a.parent;
            file   = a.file;
            line   = a.line;
        } else {
            // Re-pointed elsewhere: what this one held is dropped, and it
            // starts over as a copy of the other
            d->lo   = d->beg = a.beg;
            d->over = {};
            parent  = a.aid && a.aid!=ARENA_DEAD ? a.aid : a.parent;
        }
        arenaunlock();
    } else {
        parent = a.parent;
        file   = a.file;
        line   = a.line;
    }
    beg    = a.beg;
    end    = a.end;
    region = a.region;
    lender = a.lender;
    lentlo = a.lentlo;
    lenthi = a.lenthi;
    return *this;
}

inline Arena::~Arena()
{
    if (aid && aid!=ARENA_DEAD) {
        arenalock();
        ArenaRec *r = arenaown(this);
        if (r) {
            arenafree(r);
        }
        arenaunlock();
    }
    *(u32 volatile *)&aid = ARENA_DEAD;  // a dead store, kept on purpose
}

// Before a reservation is released: identities of arenas inside it are
// gone with it, never destroyed.
inline void arenaforget(void *lo, void *hi)
{
    arenalock();
    for (u32 i = 0; i < arenacheck.used; i++) {
        ArenaRec *r = arenacheck.recs + i;
        if (r->aid && (void *)r->self>=lo && (void *)r->self<hi) {
            arenafree(r);
        }
    }
    arenaunlock();
}
