// JSON reader and writer (RFC 8259)
//
// The reader parses a whole document into a tree in the arena. Nodes
// refer into the source (raw spans, and strings without escapes), so the
// source must outlive the tree. Each node takes about 80 bytes, so plan
// for roughly 40 times the input size in the worst case; limiting the
// input size is the caller's job. Lookups that fail return a shared
// read-only node of type JSON_MISSING, never null, so accessor chains
// need no checks along the way.
//
// The writer appends compact JSON to a Buf, placing commas and colons
// itself. Its strings are always valid UTF-8, and keys come out exactly
// in the order written, so equal inputs give byte-identical output. It
// nests no deeper than the reader accepts, so all it writes parses again.
//
// This is free and unencumbered software released into the public domain.

enum : i32 {
    JSON_MISSING,  // failed lookup or failed parse
    JSON_NULL,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT,
};

enum : i32 {
    JSON_MAXDEPTH = 64,  // nested arrays and objects, reading and writing
};

// Iterate elements or members: for (Json *e = j->child; e; e = e->next)
struct Json {
    Json *next;     // next element or member of the parent
    Json *child;    // first element or member
    iz    len;      // number of elements or members
    Str   key;      // member name, decoded
    Str   str;      // decoded string value
    Str   raw;      // source text of the value, numbers included
    i32   type;
    b32   boolean;
};

// Returned by every failed lookup. Read-only memory, so writes trap.
static Json const jsonmissing = {};

struct JsonDoc {
    Json *root;  // JSON_MISSING on failure
    Str   err;   // what went wrong
    iz    off;   // where it went wrong, as a byte offset into the source
    b32   ok;
};


// Reader

struct JsonParser {
    Arena *perm;
    Str    src;
    iz     off;
    Str    err;
};

static void jsonerror(JsonParser *p, iz off, Str err)
{
    p->off = off;
    p->err = err;
}

static void jsonskip(JsonParser *p)
{
    for (; p->off < p->src.len; p->off++) {
        u8 c = p->src[p->off];
        if (c!=' ' && c!='\t' && c!='\n' && c!='\r') {
            return;
        }
    }
}

// Four hex digits at s[i] as a number, or -1.
static i32 jsonhex4(Str s, iz i)
{
    if (s.len-i < 4) {
        return -1;
    }
    i32 r = 0;
    for (iz k = i; k < i+4; k++) {
        u8  c = lowercase(s[k]);
        i32 d = digit(c) ? c-'0' : c>='a' && c<='f' ? c-'a'+10 : -1;
        if (d < 0) {
            return -1;
        }
        r = r<<4 | d;
    }
    return r;
}

static u8 jsonunescape(u8 c)  // for the single-character escapes
{
    switch (c) {
    case 'b': return '\b';
    case 'f': return '\f';
    case 'n': return '\n';
    case 'r': return '\r';
    case 't': return '\t';
    }
    return c;  // " \ /
}

// Parse the string whose opening quote is at the current offset. Without
// escapes the result points into the source. Null on error.
static Str jsonstring(JsonParser *p)
{
    Str s   = p->src;
    iz  beg = p->off + 1;
    iz  i   = beg;
    b32 esc = 0;
    for (;;) {
        if (i == s.len) {
            jsonerror(p, i, "unterminated string");
            return {};
        }
        u8 c = s[i];
        if (c == '"') {
            break;
        } else if (c < 0x20) {
            jsonerror(p, i, "control character in string");
            return {};
        } else if (c != '\\') {
            i++;
            continue;
        }

        esc = 1;
        if (i+1 == s.len) {
            jsonerror(p, i+1, "unterminated string");
            return {};
        }
        switch (s[i+1]) {
        case '"': case '\\': case '/':
        case 'b': case 'f': case 'n': case 'r': case 't':
            i += 2;
            break;
        case 'u':
            if (jsonhex4(s, i+2) < 0) {
                jsonerror(p, i, "invalid \\u escape");
                return {};
            }
            i += 6;
            break;
        default:
            jsonerror(p, i, "invalid escape");
            return {};
        }
    }
    p->off = i + 1;

    Str raw = span(s.data+beg, s.data+i);
    if (!esc) {
        return raw;
    }

    // Escapes only shrink, so the raw length is enough.
    Str r = {allocbytes(p->perm, raw.len), 0};
    for (iz k = 0; k < raw.len;) {
        u8 c = raw[k];
        if (c != '\\') {
            r.data[r.len++] = c;
            k++;
            continue;
        }

        u8 e = raw[k+1];
        k += 2;
        if (e != 'u') {
            r.data[r.len++] = jsonunescape(e);
            continue;
        }

        i32 cp = jsonhex4(raw, k);
        k += 4;
        if (cp>=0xd800 && cp<=0xdbff && raw.len-k>=6 &&
                raw[k]=='\\' && raw[k+1]=='u') {
            i32 lo = jsonhex4(raw, k+2);
            if (lo>=0xdc00 && lo<=0xdfff) {
                cp = 0x10000 + ((cp - 0xd800)<<10) + (lo - 0xdc00);
                k += 6;
            }
        }
        if (cp>=0xd800 && cp<=0xdfff) {
            cp = 0xfffd;  // lone surrogate
        }
        r.len += utf8encode(r.data+r.len, cp);
    }

    // Return the unused tail when nothing was allocated after it.
    if ((byte *)(r.data+raw.len) == p->perm->beg) {
        p->perm->beg = (byte *)(r.data+r.len);
    }
    return r;
}

static b32 jsonnumber(JsonParser *p)
{
    Str s = p->src;
    iz  i = p->off;
    i += s[i] == '-';
    if (i==s.len || !digit(s[i])) {
        jsonerror(p, i, "invalid number");
        return 0;
    }
    if (s[i] == '0') {
        i++;
        if (i<s.len && digit(s[i])) {
            jsonerror(p, i, "leading zero in number");
            return 0;
        }
    } else {
        for (; i<s.len && digit(s[i]); i++) {}
    }

    if (i<s.len && s[i]=='.') {
        i++;
        if (i==s.len || !digit(s[i])) {
            jsonerror(p, i, "invalid number");
            return 0;
        }
        for (; i<s.len && digit(s[i]); i++) {}
    }

    if (i<s.len && (s[i]=='e' || s[i]=='E')) {
        i++;
        i += i<s.len && (s[i]=='+' || s[i]=='-');
        if (i==s.len || !digit(s[i])) {
            jsonerror(p, i, "invalid number");
            return 0;
        }
        for (; i<s.len && digit(s[i]); i++) {}
    }

    p->off = i;
    return 1;
}

static b32 jsonliteral(JsonParser *p, Str word)
{
    if (!startswith(cuthead(p->src, p->off), word)) {
        jsonerror(p, p->off, "invalid literal");
        return 0;
    }
    p->off += word.len;
    return 1;
}

// Parse a value inside depth containers. Null on error.
static Json *jsonvalue(JsonParser *p, i32 depth)
{
    jsonskip(p);
    Str s   = p->src;
    iz  beg = p->off;
    if (beg == s.len) {
        jsonerror(p, beg, "unexpected end of input");
        return 0;
    }

    Json *j = alloc<Json>(p->perm);
    switch (s[beg]) {
    case '{':
    case '[': {
        if (depth == JSON_MAXDEPTH) {
            jsonerror(p, beg, "nested too deeply");
            return 0;
        }
        b32 obj = s[beg] == '{';
        u8  end = obj ? '}' : ']';
        j->type = obj ? JSON_OBJECT : JSON_ARRAY;
        p->off++;
        jsonskip(p);
        if (p->off<s.len && s[p->off]==end) {
            p->off++;
            break;
        }

        Json **tail = &j->child;
        for (;;) {
            Str key = {};
            if (obj) {
                jsonskip(p);
                if (p->off==s.len || s[p->off]!='"') {
                    jsonerror(p, p->off, "expected string key");
                    return 0;
                }
                key = jsonstring(p);
                if (!key.data) {
                    return 0;
                }
                jsonskip(p);
                if (p->off==s.len || s[p->off]!=':') {
                    jsonerror(p, p->off, "expected ':'");
                    return 0;
                }
                p->off++;
            }

            Json *e = jsonvalue(p, depth+1);
            if (!e) {
                return 0;
            }
            e->key = key;
            *tail  = e;
            tail   = &e->next;
            j->len++;

            jsonskip(p);
            if (p->off<s.len && s[p->off]==',') {
                p->off++;
            } else if (p->off<s.len && s[p->off]==end) {
                p->off++;
                break;
            } else {
                jsonerror(p, p->off, obj ? Str("expected ',' or '}'")
                                         : Str("expected ',' or ']'"));
                return 0;
            }
        }
    } break;

    case '"':
        j->type = JSON_STRING;
        j->str  = jsonstring(p);
        if (!j->str.data) {
            return 0;
        }
        break;

    case 't':
    case 'f':
        j->type    = JSON_BOOL;
        j->boolean = s[beg] == 't';
        if (!jsonliteral(p, j->boolean ? Str("true") : Str("false"))) {
            return 0;
        }
        break;

    case 'n':
        j->type = JSON_NULL;
        if (!jsonliteral(p, "null")) {
            return 0;
        }
        break;

    default:
        if (s[beg]!='-' && !digit(s[beg])) {
            jsonerror(p, beg, "unexpected character");
            return 0;
        }
        j->type = JSON_NUMBER;
        if (!jsonnumber(p)) {
            return 0;
        }
    }

    j->raw = span(s.data+beg, s.data+p->off);
    return j;
}

// Parse a whole document: one value with optional whitespace around it.
// On failure nothing stays allocated, and the root is JSON_MISSING.
static JsonDoc jsonparse(Arena *perm, Str src)
{
    JsonDoc    r    = {};
    byte      *save = perm->beg;
    JsonParser p    = {};
    p.perm = perm;
    p.src  = src;

    Json *root = jsonvalue(&p, 0);
    if (root) {
        jsonskip(&p);
        if (p.off < src.len) {
            jsonerror(&p, p.off, "unexpected data after the value");
            root = 0;
        }
    }

    if (!root) {
        perm->beg = save;
        r.root = (Json *)&jsonmissing;
        r.err  = p.err;
        r.off  = p.off;
        return r;
    }
    r.root = root;
    r.ok   = 1;
    return r;
}


// Accessors: all accept the missing node and any type.

// Member of an object by name, the last one when names repeat.
static Json *jsonget(Json *j, Str key)
{
    Json *r = (Json *)&jsonmissing;
    if (j->type == JSON_OBJECT) {
        for (Json *m = j->child; m; m = m->next) {
            if (m->key == key) {
                r = m;
            }
        }
    }
    return r;
}

// Element i of an array, or member i of an object. Linear time: prefer
// iterating over child and next to visit them all.
static Json *jsonat(Json *j, iz i)
{
    Json *e = i<0 ? 0 : j->child;
    for (; e && i; e = e->next, i--) {}
    return e ? e : (Json *)&jsonmissing;
}

// Number of elements or members, zero for other types.
static iz jsonlen(Json *j)
{
    return j->len;
}

// Decoded string, or a null string if not a string.
static Str jsonstr(Json *j)
{
    return j->type==JSON_STRING ? j->str : Str{};
}

// Number with an integral value that fits in i64 (so 3, 3.0, and 3e0
// all give 3), otherwise dflt. Decided exactly on the decimal text, so
// 4.0000000000000001 is not an integer, whatever a double makes of it.
static i64 jsonint(Json *j, i64 dflt)
{
    if (j->type != JSON_NUMBER) {
        return dflt;
    }

    // The reader checked the grammar: -?digits(.digits)?([eE][+-]?digits)?
    // Split it into significand digits, the count of them before the
    // point, and an exponent saturated far beyond any i64.
    Str s     = j->raw;
    b32 neg   = s[0] == '-';
    iz  beg   = neg;
    iz  i     = beg;
    iz  n     = 0;
    iz  point = -1;
    for (; i<s.len && s[i]!='e' && s[i]!='E'; i++) {
        if (s[i] == '.') {
            point = n;
        } else {
            n++;
        }
    }
    point = point<0 ? n : point;
    i64 e = 0;
    if (i < s.len) {
        b32 eneg = s[++i] == '-';
        i += s[i]=='+' || s[i]=='-';
        for (; i < s.len; i++) {
            e = e<(i64)1<<32 ? e*10 + (s[i] - '0') : e;
        }
        e = eneg ? -e : e;
    }

    // Digit k of the significand is s[beg + k + (k>=point)], skipping
    // any '.', and once scaled its first whole digits are the integer
    // part. The rest must be zero, and zeros follow if it runs short.
    i64 whole = point + e;
    iz  m     = whole<0 ? 0 : whole<n ? (iz)whole : n;
    for (iz k = m; k < n; k++) {
        if (s[beg + k + (k>=point)] != '0') {
            return dflt;
        }
    }

    i64 v = 0;  // accumulated negatively, which reaches I64MIN
    for (iz k = 0; k < m; k++) {
        i32 d = s[beg + k + (k>=point)] - '0';
        if (v < (I64MIN + d)/10) {
            return dflt;
        }
        v = v*10 - d;
    }
    for (i64 k = m; k<whole && v; k++) {  // overflows within 19 steps
        if (v < I64MIN/10) {
            return dflt;
        }
        v *= 10;
    }
    if (!neg && v==I64MIN) {
        return dflt;
    }
    return neg ? v : -v;
}

// Number, or dflt if not a number. Huge magnitudes become infinite.
static f64 jsonfloat(Json *j, f64 dflt)
{
    if (j->type != JSON_NUMBER) {
        return dflt;
    }
    Float f = parsefloat(j->raw);
    return f.ok ? f.val : dflt;
}

static b32 jsonbool(Json *j, b32 dflt)
{
    return j->type==JSON_BOOL ? j->boolean : dflt;
}


// Writer

// Usage: JsonOut w = {&buf}; putobject(&w); putkey(&w, "k"); ...
// Misuse (a value without a key in an object, unbalanced ends, nesting
// past JSON_MAXDEPTH by hand) is a bug caught by assertions.
struct JsonOut {
    Buf *b       = {};
    u64  objects = {};  // bit per open container, set for objects
    i32  depth   = {};
    b32  comma   = {};  // the next element or member needs a comma
    b32  haskey  = {};  // a key was written and awaits its value
};

// Decode one code point at s[*i], strictly: overlong forms, values past
// U+10FFFF, and truncated sequences return -1, advancing past the
// longest valid prefix (at least one byte). Surrogates (WTF-8) decode
// as themselves for the caller to judge.
static i32 utf8strict(Str s, iz *i)
{
    u8 c = s[(*i)++];
    if (c < 0x80) {
        return c;
    }
    i32 n = c>=0xc2 && c<=0xdf ? 1 : c>=0xe0 && c<=0xef ? 2 :
            c>=0xf0 && c<=0xf4 ? 3 : 0;
    if (!n) {
        return -1;
    }
    u8  lo = c==0xe0 ? 0xa0 : c==0xf0 ? 0x90 : 0x80;
    u8  hi = c==0xf4 ? 0x8f : 0xbf;
    i32 r  = c & (0x3f >> n);
    for (i32 k = 0; k < n; k++) {
        if (*i == s.len) {
            return -1;
        }
        u8 b = s[*i];
        if (b<lo || b>hi) {
            return -1;
        }
        lo = 0x80;
        hi = 0xbf;
        r  = r<<6 | (b & 0x3f);
        (*i)++;
    }
    return r;
}

// Print s as a JSON string literal. Invalid UTF-8 and lone surrogates
// become U+FFFD, while a surrogate pair encoded as two halves (WTF-8
// concatenation) is joined into its code point.
static void printjsonstr(Buf *b, Str s)
{
    putbyte(b, '"');
    iz last = 0;
    for (iz i = 0; i < s.len;) {
        iz  beg = i;
        u8  c   = s[i];
        u8  tmp[6];
        Str rep = {};
        if (c >= 0x80) {
            i32 cp = utf8strict(s, &i);
            if (cp>=0 && (cp<0xd800 || cp>0xdfff)) {
                continue;  // valid, copied with its run
            }
            if (cp>=0xd800 && cp<=0xdbff && i<s.len) {
                iz  j  = i;
                i32 lo = utf8strict(s, &j);
                if (lo>=0xdc00 && lo<=0xdfff) {
                    cp = 0x10000 + ((cp - 0xd800)<<10) + (lo - 0xdc00);
                    i  = j;
                }
            }
            if (cp<0 || (cp>=0xd800 && cp<=0xdfff)) {
                cp = 0xfffd;
            }
            rep = {tmp, utf8encode(tmp, cp)};
        } else if (c>=0x20 && c!='"' && c!='\\') {
            i++;
            continue;
        } else {
            i++;
            switch (c) {
            case '"':  rep = "\\\""; break;
            case '\\': rep = "\\\\"; break;
            case '\b': rep = "\\b";  break;
            case '\f': rep = "\\f";  break;
            case '\n': rep = "\\n";  break;
            case '\r': rep = "\\r";  break;
            case '\t': rep = "\\t";  break;
            default:
                tmp[0] = '\\';
                tmp[1] = 'u';
                tmp[2] = '0';
                tmp[3] = '0';
                tmp[4] = (u8)"0123456789abcdef"[c>>4];
                tmp[5] = (u8)"0123456789abcdef"[c&15];
                rep = {tmp, 6};
            }
        }
        print(b, span(s.data+last, s.data+beg));
        print(b, rep);
        last = i;
    }
    print(b, cuthead(s, last));
    putbyte(b, '"');
}

// v * 10^e, exact for |e| <= 22 and close beyond.
static f64 jsonscale(f64 v, i32 e)
{
    static f64 const pow10[] = {
        1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
        1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22,
    };
    for (; e >  22; e -= 22) v *= 1e22;
    for (; e < -22; e += 22) v /= 1e22;
    return e<0 ? v/pow10[-e] : v*pow10[e];
}

// Print a finite number in its shortest form of at most 15 significant
// digits. From 1e-8 up to 1e37 the scaling is a single rounding, so any
// value written with 15 or fewer digits comes back exactly; beyond that
// the last digit may be off. Like JavaScript, plain notation from 1e-6
// up to 1e21, else an exponent.
static void printjsonnum(Buf *b, f64 v)
{
    if (v == 0) {
        putbyte(b, '0');  // including negative zero
        return;
    }
    if (v < 0) {
        putbyte(b, '-');
        v = -v;
    }

    // Estimate the decimal exponent from the binary exponent, then
    // correct it, which takes at most one step either way.
    u64 bits;
    __builtin_memcpy(&bits, &v, 8);
    i32 e2 = (i32)(bits >> 52) - 1023;
    if (e2 == -1023) {
        e2 = -1074 + 63 - __builtin_clzll(bits);  // subnormal
    }
    i32 e10 = (e2 * 78913) >> 18;  // floor(e2 * log10(2)), near enough
    f64 x   = jsonscale(v, 14-e10);
    if (x >= 1e15) {
        x = jsonscale(v, 14 - ++e10);
    } else if (x < 1e14) {
        x = jsonscale(v, 14 - --e10);
    }
    u64 m = (u64)(x + 0.5);
    if (m >= 1000000000000000) {
        m /= 10;  // rounded up to the next power of ten
        e10++;
    }

    u8  digits[15];
    i32 n = 15;
    for (; m%10 == 0; n--) {
        m /= 10;
    }
    for (i32 i = n-1; i >= 0; i--) {
        digits[i] = (u8)('0' + m%10);
        m /= 10;
    }

    if (e10<-6 || e10>20) {
        putbyte(b, digits[0]);
        if (n > 1) {
            putbyte(b, '.');
            print(b, Str{digits+1, n-1});
        }
        putbyte(b, 'e');
        print(b, (i64)e10);
    } else if (e10 < 0) {
        print(b, "0.");
        for (i32 i = -1; i > e10; i--) {
            putbyte(b, '0');
        }
        print(b, Str{digits, n});
    } else {
        for (i32 i = 0; i <= e10 || i < n; i++) {
            if (i == e10+1) {
                putbyte(b, '.');
            }
            putbyte(b, i<n ? digits[i] : '0');
        }
    }
}

// Separator bookkeeping before a value.
static void putsep(JsonOut *w)
{
    b32 inobject = w->depth && (w->objects>>(w->depth-1) & 1);
    assert(inobject == w->haskey);
    if (w->comma && !w->haskey && w->depth) {
        putbyte(w->b, ',');
    }
    w->haskey = 0;
    w->comma  = 1;
}

static void putopen(JsonOut *w, b32 obj)
{
    static_assert(JSON_MAXDEPTH <= 64);  // one bit each in objects
    assert(w->depth < JSON_MAXDEPTH);
    putsep(w);
    putbyte(w->b, obj ? '{' : '[');
    u64 bit = (u64)1 << w->depth;
    w->objects = obj ? w->objects|bit : w->objects&~bit;
    w->depth++;
    w->comma = 0;
}

static void putobject(JsonOut *w)
{
    putopen(w, 1);
}

static void putarray(JsonOut *w)
{
    putopen(w, 0);
}

// Close the innermost object or array.
static void putend(JsonOut *w)
{
    assert(w->depth>0 && !w->haskey);
    w->depth--;
    putbyte(w->b, w->objects>>w->depth & 1 ? '}' : ']');
    w->comma = 1;
}

static void putkey(JsonOut *w, Str key)
{
    assert(w->depth && (w->objects>>(w->depth-1) & 1) && !w->haskey);
    if (w->comma) {
        putbyte(w->b, ',');
    }
    printjsonstr(w->b, key);
    putbyte(w->b, ':');
    w->haskey = 1;
}

static void putstr(JsonOut *w, Str s)
{
    putsep(w);
    printjsonstr(w->b, s);
}

static void putint(JsonOut *w, i64 v)
{
    putsep(w);
    print(w->b, v);
}

// Non-finite values have no JSON form and become null.
static void putfloat(JsonOut *w, f64 v)
{
    putsep(w);
    if (v-v != 0) {
        print(w->b, "null");
    } else {
        printjsonnum(w->b, v);
    }
}

static void putbool(JsonOut *w, b32 v)
{
    putsep(w);
    print(w->b, v ? Str("true") : Str("false"));
}

static void putnull(JsonOut *w)
{
    putsep(w);
    print(w->b, "null");
}

// Insert one complete, pre-encoded JSON value as is. The caller vouches
// for it: valid, shallow enough to stay within JSON_MAXDEPTH where it
// lands, and without newlines when building records. For a parsed value
// (rather than its raw text) putjson ensures all of this.
static void putraw(JsonOut *w, Str json)
{
    assert(json.len > 0);
    putsep(w);
    print(w->b, json);
}

// Write a parsed value in canonical form: compact, members in source
// order (duplicates kept), strings escaped by printjsonstr, and numbers
// as their source text. The missing node writes as null, and so does
// any array or object that would nest past JSON_MAXDEPTH, counting the
// containers already open: a document the reader just accepted may sit
// inside a request or record, and the result must parse again.
static void putjson(JsonOut *w, Json *j)
{
    switch (j->type) {
    case JSON_MISSING:
    case JSON_NULL:   putnull(w);                break;
    case JSON_BOOL:   putbool(w, j->boolean);    break;
    case JSON_NUMBER: putraw(w, j->raw);         break;
    case JSON_STRING: putstr(w, j->str);         break;
    case JSON_ARRAY:
    case JSON_OBJECT:
        if (w->depth == JSON_MAXDEPTH) {
            putnull(w);
            break;
        }
        putopen(w, j->type == JSON_OBJECT);
        for (Json *e = j->child; e; e = e->next) {
            if (j->type == JSON_OBJECT) {
                putkey(w, e->key);
            }
            putjson(w, e);
        }
        putend(w);
    }
}
