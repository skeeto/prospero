// Foundation: types, arenas, strings, buffers, maps, numbers, sorting
// Derived from ssg's base.cpp (skeeto.github.com/_ssg).
// This is free and unencumbered software released into the public domain.

using u8   = unsigned char;
using b32  = decltype(0);
using i32  = decltype(0);
using u32  = decltype(0u);
using i64  = decltype(0ll);
using u64  = decltype(0ull);
using iz   = decltype((u8 *)0 - (u8 *)0);
using uz   = decltype(sizeof(0));
using byte = decltype('0');
using f32  = decltype(0.f);
using f64  = decltype(0.);

#define assert(c)   while (!(c)) __builtin_unreachable()
#define countof(a)  (iz)(sizeof(a) / sizeof(*(a)))

enum : i64 {
    I64MAX = (i64)((u64)-1 >> 1),
    I64MIN = -I64MAX - 1,
};

template<typename T>
void *operator new(uz, T *p) noexcept { return p; }

// A reserved range of address space, committed on demand. The platform
// places this header at the start of the reservation (os_reserve).
struct Region {
    byte *base;
    iz    cap;        // reserved bytes from base
    iz    committed;  // usable bytes from base
};

// Arenas with a region grow by committing more of it. Only the arena at
// the top of a region may grow: when splitting one, clear the region on
// every part but the last.
#if PROSPERO_ARENACHECK
// Where an arena came to be, for the checker's reports (arenacheck.cpp)
#  define ARENASITE , char const *file = __builtin_FILE(), i32 line = __builtin_LINE()
#else
#  define ARENASITE
#endif

struct Arena {
    byte   *beg    = {};
    byte   *end    = {};
    Region *region = {};
#if PROSPERO_ARENACHECK
    // The checker's view: an identity once it allocates, the arena it
    // was copied from, and the block a carve was lent by its parent.
    mutable u32  aid    = 0;
    u32          parent = 0;
    u32          lender = 0;
    byte        *lentlo = {};
    byte        *lenthi = {};
    char const  *file   = {};
    i32          line   = 0;

    constexpr Arena(char const *file = __builtin_FILE(), i32 line = __builtin_LINE())
        : file{file}, line{line} {}
    constexpr Arena(byte *beg, byte *end, Region *region = 0 ARENASITE)
        : beg{beg}, end{end}, region{region}, file{file}, line{line} {}
    Arena(Arena const &, char const * = __builtin_FILE(), i32 = __builtin_LINE());
    Arena &operator=(Arena const &);
    ~Arena();
#endif
};

// Provided by the platform layer: report out-of-memory and exit.
[[noreturn]] static void os_oom();
// Provided by the platform layer: commit a region from its base up to
// the given size, updating committed. Returns 0 on failure.
static b32 os_commit(Region *, iz upto);

#if PROSPERO_ARENACHECK
inline void arenacheck_alloc(Arena *, byte *, iz, void *pc);
#endif

// Extend a region-backed arena so that count objects of the given size
// fit after pad bytes, or report out-of-memory.
static void arenagrow(Arena *a, iz pad, iz count, iz size)
{
    Region *r = a->region;
    if (!r || count >= (r->base + r->cap - a->beg - pad)/size) {
        os_oom();
    }
    iz grain = (iz)1 << 20;
    iz upto  = a->beg - r->base + pad + (count+1)*size;
    upto += -upto & (grain - 1);
    upto  = upto<r->cap ? upto : r->cap;
    if (upto>r->committed && !os_commit(r, upto)) {
        os_oom();
    }
    a->end = r->base + r->committed;
}

// Whether need more bytes can be allocated, counting uncommitted space.
static b32 fits(Arena *a, iz need)
{
    byte *limit = a->region ? a->region->base+a->region->cap : a->end;
    return need>=0 && need<=limit-a->beg;
}

template<typename T>
static T *alloc(Arena *a, iz count = 1)
{
    iz size = sizeof(T);
    iz pad  = -(uz)a->beg & (alignof(T) - 1);
    if (count >= (a->end - a->beg - pad)/size) {
        arenagrow(a, pad, count, size);
    }
    T *r = (T *)(a->beg + pad);
#if PROSPERO_ARENACHECK
    arenacheck_alloc(a, (byte *)r, count*size, __builtin_return_address(0));
#endif
    a->beg += pad + count*size;
    for (iz i = 0; i < count; i++) {
        new (r+i) T();
    }
    return r;
}

// Like alloc<u8>, but does not zero the memory.
static u8 *allocbytes(Arena *a, iz count)
{
    if (count > a->end - a->beg) {
        arenagrow(a, 0, count, 1);
    }
    u8 *r = (u8 *)a->beg;
#if PROSPERO_ARENACHECK
    arenacheck_alloc(a, a->beg, count, __builtin_return_address(0));
#endif
    a->beg += count;
    return r;
}

// A scratch arena of cap bytes taken off the front of a, for a callee
// whose results go in a itself: passing a as both, by pointer and by
// value, would have its scratch overwrite results made before it was
// done. It cannot grow.
static Arena carve(Arena *a, iz cap ARENASITE)
{
    Arena r = {};
    r.beg = (byte *)allocbytes(a, cap);
    r.end = r.beg + cap;
#if PROSPERO_ARENACHECK
    r.lender = a->aid;
    r.lentlo = r.beg;
    r.lenthi = r.end;
    r.file   = file;
    r.line   = line;
#endif
    return r;
}

// A growable arena over a reservation, or a null arena given null.
static Arena newarena(Region *r ARENASITE)
{
    Arena a = {};
    if (r) {
        a.beg    = r->base;
        a.end    = r->base + r->committed;
        a.region = r;
    }
#if PROSPERO_ARENACHECK
    a.file = file;
    a.line = line;
#endif
    return a;
}

static void copybytes(void *dst, void *src, iz len)
{
    if (len) __builtin_memcpy(dst, src, (uz)len);
}


// Strings

struct Str {
    union {
        u8         *data = 0;
        char const *cdata;
    };
    iz len = 0;

    Str() = default;

    constexpr Str(u8 *data, iz len) : data{data}, len{len} {}

    template<iz N>
    constexpr Str(char const (&s)[N]) : cdata{s}, len{N-1} {}

    u8 &operator[](iz i)
    {
        assert(i >= 0 && i < len);
        return data[i];
    }

    bool operator==(Str s)
    {
        return len==s.len && (!len || !__builtin_memcmp(data, s.data, (uz)len));
    }
};

static Str span(u8 *beg, u8 *end)
{
    return {beg, end - beg};
}

static Str takehead(Str s, iz n)
{
    assert(n >= 0 && n <= s.len);
    s.len = n;
    return s;
}

static Str cuthead(Str s, iz n)
{
    assert(n >= 0 && n <= s.len);
    s.data += n;
    s.len  -= n;
    return s;
}

static Str taketail(Str s, iz n)
{
    return cuthead(s, s.len-n);
}

static Str cuttail(Str s, iz n)
{
    return takehead(s, s.len-n);
}

static b32 startswith(Str s, Str prefix)
{
    return s.len>=prefix.len && takehead(s, prefix.len)==prefix;
}

static b32 endswith(Str s, Str suffix)
{
    return s.len>=suffix.len && taketail(s, suffix.len)==suffix;
}

static i32 compare(Str a, Str b)  // like memcmp, byte-wise
{
    iz len = a.len<b.len ? a.len : b.len;
    for (iz i = 0; i < len; i++) {
        i32 d = a[i] - b[i];
        if (d) return d;
    }
    return a.len<b.len ? -1 : a.len>b.len;
}

struct Cut {
    Str head;
    Str tail;
    b32 ok;
};

static Cut cut(Str s, u8 c)
{
    Cut r = {};
    iz  i = 0;
    for (; i < s.len && s[i] != c; i++) {}
    r.ok   = i < s.len;
    r.head = takehead(s, i);
    r.tail = cuthead(s, i+r.ok);
    return r;
}

static iz find(Str s, Str needle)  // -1 if not found
{
    if (!needle.len) {
        return 0;
    } else if (needle.len > s.len) {
        return -1;  // and no pointer before the start
    }
    u8 *last = s.data + s.len - needle.len;
    for (u8 *p = s.data; p <= last; p++) {
        if (*p==needle.data[0] && !__builtin_memcmp(p, needle.data, (uz)needle.len)) {
            return p - s.data;
        }
    }
    return -1;
}

static b32 whitespace(u8 c)
{
    return c==' ' || c=='\t' || c=='\n' || c=='\r' || c=='\f' || c=='\v';
}

static b32 digit(u8 c)
{
    return c>='0' && c<='9';
}

static b32 letter(u8 c)
{
    return (c>='a' && c<='z') || (c>='A' && c<='Z');
}

static b32 alnum(u8 c)
{
    return letter(c) || digit(c);
}

static u8 lowercase(u8 c)
{
    return c>='A' && c<='Z' ? (u8)(c + 32) : c;
}

// Equality ignoring ASCII case.
static b32 equalfold(Str a, Str b)
{
    if (a.len != b.len) {
        return 0;
    }
    for (iz i = 0; i < a.len; i++) {
        if (lowercase(a[i]) != lowercase(b[i])) {
            return 0;
        }
    }
    return 1;
}

static Str trimleft(Str s)
{
    for (; s.len && whitespace(*s.data); s.data++, s.len--) {}
    return s;
}

static Str trimright(Str s)
{
    for (; s.len && whitespace(s.data[s.len-1]); s.len--) {}
    return s;
}

static Str trim(Str s)
{
    return trimleft(trimright(s));
}

static Str clone(Arena *a, Str s)
{
    Str r = s;
    r.data = allocbytes(a, s.len);
    copybytes(r.data, s.data, s.len);
    return r;
}

// Concatenate, extending head in place when it sits at the arena's end.
static Str concat(Arena *a, Str head, Str tail)
{
    if (!head.data || (byte *)(head.data+head.len) != a->beg) {
        head = clone(a, head);
    }
    head.len += clone(a, tail).len;
    return head;
}

// Join path components with a slash. An empty or "." directory is
// omitted.
static Str join(Arena *a, Str dir, Str name)
{
    if (!dir.len || dir == ".") {
        return name;
    }
    Str r = concat(a, dir, "/");
    return concat(a, r, name);
}

static u64 hash(Str s)
{
    u64 h = 0x100;
    for (iz i = 0; i < s.len; i++) {
        h ^= s[i];
        h *= 1111111111111111111u;
    }
    return h;
}

// Decode one UTF-8 code point at s[*i], advancing *i. Invalid bytes
// decode as themselves (Latin-1), one byte at a time.
static i32 utf8decode(Str s, iz *i)
{
    u8  c = s[(*i)++];
    i32 n = c>=0xf0 ? 3 : c>=0xe0 ? 2 : c>=0xc0 ? 1 : 0;
    if (!n || *i+n > s.len) {
        return c;
    }
    i32 r = c & (0x3f >> n);
    for (i32 k = 0; k < n; k++) {
        u8 b = s[*i+k];
        if ((b & 0xc0) != 0x80) {
            return c;
        }
        r = r<<6 | (b & 0x3f);
    }
    *i += n;
    return r;
}

// Code point ending just before s[i], or -1 at the start.
static i32 utf8prev(Str s, iz i, iz *start)
{
    if (i <= 0) {
        *start = 0;
        return -1;
    }
    iz j = i - 1;
    for (i32 k = 0; k < 3 && j > 0 && (s[j]&0xc0) == 0x80; k++, j--) {}
    iz end = j;
    i32 r = utf8decode(s, &end);
    if (end != i) {
        j = i - 1;
        r = s[j];
    }
    *start = j;
    return r;
}

// Encode a code point (at most 0x10ffff) as UTF-8, returning the length.
static iz utf8encode(u8 *s, i32 c)
{
    assert(c>=0 && c<=0x10ffff);
    if (c < 0x80) {
        s[0] = (u8)c;
        return 1;
    } else if (c < 0x800) {
        s[0] = (u8)(0xc0 | c>>6);
        s[1] = (u8)(0x80 | (c    & 63));
        return 2;
    } else if (c < 0x10000) {
        s[0] = (u8)(0xe0 | c>>12);
        s[1] = (u8)(0x80 | (c>>6 & 63));
        s[2] = (u8)(0x80 | (c    & 63));
        return 3;
    }
    s[0] = (u8)(0xf0 | c>>18);
    s[1] = (u8)(0x80 | (c>>12 & 63));
    s[2] = (u8)(0x80 | (c>>6  & 63));
    s[3] = (u8)(0x80 | (c     & 63));
    return 4;
}


// Dynamic arrays

template<typename T>
struct Slice {
    T *data = {};
    iz len  = {};
    iz cap  = {};

    T &operator[](iz i)
    {
        assert(i >= 0 && i < len);
        return data[i];
    }
};

template<typename T>
static Slice<T> push(Arena *a, Slice<T> s, T v)
{
    if (s.len == s.cap) {
        if (!s.data || (byte *)(s.data+s.cap) != a->beg) {
            T *data = alloc<T>(a, s.cap);
            for (iz i = 0; i < s.len; i++) {
                data[i] = s.data[i];
            }
            s.data = data;
        }
        iz extend = s.cap ? s.cap : 8;
        alloc<T>(a, extend);
        s.cap += extend;
    }
    s.data[s.len++] = v;
    return s;
}


// Hash trie keyed by strings. With a null arena, lookup only.

template<typename V>
struct Map {
    Map *child[4];
    Str  key;
    V    val;
};

template<typename V>
static V *upsert(Map<V> **m, Str key, Arena *a)
{
    for (u64 h = hash(key); *m; h <<= 2) {
        if ((*m)->key == key) {
            return &(*m)->val;
        }
        m = &(*m)->child[h>>62];
    }
    if (!a) {
        return 0;
    }
    *m = alloc<Map<V>>(a);
    (*m)->key = key;
    return &(*m)->val;
}


// Output buffers: appending grows in the arena, in place when possible.

struct Buf {
    u8    *data = {};
    iz     len  = {};
    iz     cap  = {};
    Arena *a    = {};

    Buf() = default;
    Buf(Arena *a, iz cap = 1<<12) : data{allocbytes(a, cap)}, cap{cap}, a{a} {}
};

static void reserve(Buf *b, iz need)
{
    if (b->cap - b->len >= need) {
        return;
    }
    Arena *a = b->a;
    iz extend = b->cap > need ? b->cap : need;
    if ((byte *)(b->data+b->cap) == a->beg) {
        allocbytes(a, extend);
    } else {
        u8 *data = allocbytes(a, b->cap+extend);
        copybytes(data, b->data, b->len);
        b->data = data;
    }
    b->cap += extend;
}

static void print(Buf *b, Str s)
{
    reserve(b, s.len);
    copybytes(b->data+b->len, s.data, s.len);
    b->len += s.len;
}

static void putbyte(Buf *b, u8 c)
{
    reserve(b, 1);
    b->data[b->len++] = c;
}

static void print(Buf *b, i64 v)
{
    u8  tmp[32];
    u8 *end = tmp + countof(tmp);
    u8 *beg = end;
    i64 t   = v<0 ? v : -v;
    do {
        *--beg = (u8)('0' - t%10);
    } while (t /= 10);
    if (v < 0) {
        *--beg = '-';
    }
    print(b, span(beg, end));
}

// Zero-padded decimal of at least width digits.
static void printpad(Buf *b, i64 v, i32 width)
{
    i64 limit = 1;
    for (i32 i = 1; i < width; i++) {
        limit *= 10;
        if (v < limit) putbyte(b, '0');
    }
    print(b, v);
}

static void printhex(Buf *b, u64 v, i32 digits)
{
    for (i32 i = digits-1; i >= 0; i--) {
        putbyte(b, "0123456789abcdef"[v>>(4*i) & 15]);
    }
}

// A moment in UTC, from epoch milliseconds: the proleptic Gregorian
// calendar's date, and the time of day.
struct Civil {
    i64 year, month, day;
    i64 hour, min, sec;
};

static Civil civil(i64 ms)
{
    i64 secs = (ms>=0 ? ms : ms-999) / 1000;
    i64 days = (secs>=0 ? secs : secs-86399) / 86400;
    i64 rem  = secs - days*86400;
    i64 z    = days + 719468;
    i64 era  = (z >= 0 ? z : z - 146096) / 146097;
    i64 doe  = z - era*146097;
    i64 yoe  = (doe - doe/1460 + doe/36524 - doe/146096) / 365;
    i64 doy  = doe - (365*yoe + yoe/4 - yoe/100);
    i64 mp   = (5*doy + 2)/153;
    Civil c  = {};
    c.day   = doy - (153*mp + 2)/5 + 1;
    c.month = mp<10 ? mp+3 : mp-9;
    c.year  = yoe + era*400 + (c.month <= 2);
    c.hour  = rem / 3600;
    c.min   = rem / 60 % 60;
    c.sec   = rem % 60;
    return c;
}

// ISO 8601 in UTC, to the second: 2026-10-26T14:05:09Z.
static void printiso(Buf *b, i64 ms)
{
    Civil c = civil(ms);
    printpad(b, c.year, 4);
    putbyte(b, '-');
    printpad(b, c.month, 2);
    putbyte(b, '-');
    printpad(b, c.day, 2);
    putbyte(b, 'T');
    printpad(b, c.hour, 2);
    putbyte(b, ':');
    printpad(b, c.min, 2);
    putbyte(b, ':');
    printpad(b, c.sec, 2);
    putbyte(b, 'Z');
}

// Escape & < > for HTML text content.
static void printhtml(Buf *b, Str s)
{
    iz last = 0;
    for (iz i = 0; i < s.len; i++) {
        Str rep = {};
        switch (s[i]) {
        case '&': rep = "&amp;"; break;
        case '<': rep = "&lt;";  break;
        case '>': rep = "&gt;";  break;
        default: continue;
        }
        print(b, span(s.data+last, s.data+i));
        print(b, rep);
        last = i + 1;
    }
    print(b, cuthead(s, last));
}

// Escape & < > " for double-quoted HTML attributes.
static void printattr(Buf *b, Str s)
{
    iz last = 0;
    for (iz i = 0; i < s.len; i++) {
        Str rep = {};
        switch (s[i]) {
        case '&': rep = "&amp;";  break;
        case '<': rep = "&lt;";   break;
        case '>': rep = "&gt;";   break;
        case '"': rep = "&quot;"; break;
        default: continue;
        }
        print(b, span(s.data+last, s.data+i));
        print(b, rep);
        last = i + 1;
    }
    print(b, cuthead(s, last));
}

static Str finish(Buf *b)
{
    return {b->data, b->len};
}


// Numbers

struct Int {
    i64 val;
    b32 ok;
};

// Parse a whole string as a decimal integer with an optional minus sign,
// failing on overflow.
static Int parseint(Str s)
{
    Int r   = {};
    b32 neg = s.len && s[0]=='-';
    iz  i   = neg;
    if (i == s.len) {
        return r;
    }
    i64 v = 0;  // accumulated negatively, which reaches I64MIN
    for (; i < s.len; i++) {
        if (!digit(s[i])) {
            return r;
        }
        i32 d = s[i] - '0';
        if (v < (I64MIN + d)/10) {
            return r;
        }
        v = v*10 - d;
    }
    if (!neg && v==I64MIN) {
        return r;
    }
    r.val = neg ? v : -v;
    r.ok  = 1;
    return r;
}

struct Float {
    f64 val;
    b32 ok;
};

// Parse a whole string as a JSON number. Keeps 19 significant digits and
// scales by exact powers of ten, so results are close but not always
// correctly rounded. Out-of-range values become zero or infinity.
static Float parsefloat(Str s)
{
    static f64 const pow10[] = {
        1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
        1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22,
    };

    Float r   = {};
    b32   neg = s.len && s[0]=='-';
    iz    i   = neg;
    if (i==s.len || !digit(s[i])) {
        return r;
    }

    u64 m = 0;  // significant digits
    i32 n = 0;  // number of significant digits kept, at most 19
    i32 e = 0;  // decimal exponent of m
    if (s[i] == '0') {
        i++;
    } else {
        for (; i<s.len && digit(s[i]); i++) {
            if (n < 19) {
                m = m*10 + (u64)(s[i] - '0');
                n++;
            } else {
                e++;
            }
        }
    }

    if (i<s.len && s[i]=='.') {
        i++;
        if (i==s.len || !digit(s[i])) {
            return r;
        }
        for (; i<s.len && digit(s[i]); i++) {
            if (n < 19) {
                m = m*10 + (u64)(s[i] - '0');
                n += m != 0;  // leading zeros are not significant
                e--;
            }
        }
    }

    if (i<s.len && (s[i]=='e' || s[i]=='E')) {
        i++;
        b32 eneg = i<s.len && s[i]=='-';
        i += i<s.len && (s[i]=='-' || s[i]=='+');
        if (i==s.len || !digit(s[i])) {
            return r;
        }
        i32 x = 0;
        for (; i<s.len && digit(s[i]); i++) {
            x = x<100000 ? x*10 + (s[i] - '0') : x;  // saturate
        }
        e += eneg ? -x : x;
    }

    if (i != s.len) {
        return r;
    }

    f64 v = (f64)m;
    if (m) {
        for (; e >  22; e -= 22) v *= 1e22;
        for (; e < -22; e += 22) v /= 1e22;
        v = e<0 ? v/pow10[-e] : v*pow10[e];
    }
    r.val = neg ? -v : v;
    r.ok  = 1;
    return r;
}

// Fixed-point decimal with at most the given number of decimals (0 to
// 9), dropping trailing zeros. Magnitudes that would not fit switch to a
// mantissa and exponent. Non-finite values print as nan, inf, or -inf.
static void printfloat(Buf *b, f64 v, i32 decimals)
{
    assert(decimals>=0 && decimals<=9);
    if (v != v) {
        print(b, "nan");
        return;
    }
    if (v < 0) {
        putbyte(b, '-');
        v = -v;
    }
    if (v-v != 0) {
        print(b, "inf");
        return;
    }

    i64 scale = 1;
    for (i32 i = 0; i < decimals; i++) {
        scale *= 10;
    }
    i32 exp = 0;
    if (v*(f64)scale >= 1e18) {
        for (; v >= 10; exp++) {
            v /= 10;
        }
    }
    u64 x = (u64)(v*(f64)scale + 0.5);  // v*scale < 1e18 here
    if (exp && x>=10*(u64)scale) {
        x /= 10;  // rounded up to 10.0
        exp++;
    }

    print(b, (i64)(x / (u64)scale));
    u64 frac = x % (u64)scale;
    if (frac) {
        i32 digits = decimals;
        for (; frac%10 == 0; digits--) {
            frac /= 10;
        }
        putbyte(b, '.');
        printpad(b, (i64)frac, digits);
    }
    if (exp) {
        putbyte(b, 'e');
        print(b, (i64)exp);
    }
}


// Stable merge sort

template<typename T>
static void sort(T *a, iz n, b32 (*less)(T *, T *), Arena scratch)
{
    if (n < 2) {
        return;
    }
    T *tmp = alloc<T>(&scratch, n);
    for (iz width = 1; width < n; width *= 2) {
        for (iz lo = 0; lo < n; lo += 2*width) {
            iz mid = lo+width < n ? lo+width : n;
            iz hi  = lo+2*width < n ? lo+2*width : n;
            iz i = lo, j = mid, k = lo;
            while (i < mid && j < hi) {
                tmp[k++] = less(a+j, a+i) ? a[j++] : a[i++];
            }
            while (i < mid) tmp[k++] = a[i++];
            while (j < hi)  tmp[k++] = a[j++];
        }
        for (iz i = 0; i < n; i++) {
            a[i] = tmp[i];
        }
    }
}


#if PROSPERO_ARENACHECK
#  include "arenacheck.cpp"
#endif
