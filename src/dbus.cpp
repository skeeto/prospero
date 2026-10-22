// D-Bus messages: built and parsed, in little-endian wire format
//
// Just enough of the D-Bus specification for a client: a body is built
// value by value, its signature written out by the caller, and a whole
// message put together around it; a message read back is split into its
// header fields and its body, which parses against its signature into a
// tree of values. The transport (a socket, authentication, passed file
// descriptors) is the platform's: see deskwayland.cpp.
//
// Lookups that fail return a shared empty value of type 0, never null,
// so accessor chains need no checks along the way, as with JSON.
//
// This is free and unencumbered software released into the public domain.

enum : u8 {
    DB_CALL   = 1,
    DB_RETURN = 2,
    DB_ERROR  = 3,
    DB_SIGNAL = 4,
};

enum : u8 {
    DB_NOREPLY = 1,  // flags: no reply wanted
};

enum : u8 {
    DBF_PATH = 1, DBF_IFACE, DBF_MEMBER, DBF_ERROR, DBF_REPLYTO,
    DBF_DEST, DBF_SENDER, DBF_SIG, DBF_FDS,
};

enum : iz {
    DB_MSGMAX   = (iz)1 << 27,  // the specification's limit
    DB_MAXDEPTH = 32,
};


// Building a body

struct DbArray {
    iz lenat;  // where its length goes
    iz start;  // its first element
};

static void dbpad(Buf *b, iz align)
{
    while (b->len & (align-1)) {
        putbyte(b, 0);
    }
}

static void dbraw(Buf *b, void *p, iz len)
{
    print(b, Str{(u8 *)p, len});
}

static void dbu8(Buf *b, u8 v)
{
    putbyte(b, v);
}

static void dbu32(Buf *b, u32 v)
{
    dbpad(b, 4);
    u8 p[4] = {(u8)v, (u8)(v>>8), (u8)(v>>16), (u8)(v>>24)};
    dbraw(b, p, 4);
}

static void dbi32(Buf *b, i32 v)
{
    dbu32(b, (u32)v);
}

static void dbbool(Buf *b, b32 v)
{
    dbu32(b, v ? 1 : 0);
}

static void dbu64(Buf *b, u64 v)
{
    dbpad(b, 8);
    u8 p[8];
    for (i32 i = 0; i < 8; i++) {
        p[i] = (u8)(v >> (8*i));
    }
    dbraw(b, p, 8);
}

static void dbf64(Buf *b, f64 v)
{
    u64 u = 0;
    __builtin_memcpy(&u, &v, 8);
    dbu64(b, u);
}

// A string or an object path ('s', 'o').
static void dbstr(Buf *b, Str s)
{
    dbu32(b, (u32)s.len);
    print(b, s);
    putbyte(b, 0);
}

// A signature ('g'), at most 255 bytes.
static void dbsig(Buf *b, Str s)
{
    putbyte(b, (u8)s.len);
    print(b, s);
    putbyte(b, 0);
}

// Start an array whose elements align to align: 8 for structs and dict
// entries, else the element's own.
static DbArray dbarray(Buf *b, iz align)
{
    DbArray r = {};
    dbu32(b, 0);
    r.lenat = b->len - 4;
    dbpad(b, align);
    r.start = b->len;
    return r;
}

static void dbarrayend(Buf *b, DbArray a)
{
    u32 n = (u32)(b->len - a.start);
    u8 *p = b->data + a.lenat;
    p[0] = (u8)n;
    p[1] = (u8)(n >> 8);
    p[2] = (u8)(n >> 16);
    p[3] = (u8)(n >> 24);
}

// A struct or a dict entry begins aligned to 8.
static void dbstruct(Buf *b)
{
    dbpad(b, 8);
}

// Entries of an a{sv}, the options of most calls, after dbarray(b, 8).
static void dbkey(Buf *b, Str key, Str sig)
{
    dbstruct(b);
    dbstr(b, key);
    dbsig(b, sig);
}

static void dbentrystr(Buf *b, Str key, Str v)
{
    dbkey(b, key, "s");
    dbstr(b, v);
}

static void dbentryu32(Buf *b, Str key, u32 v)
{
    dbkey(b, key, "u");
    dbu32(b, v);
}

static void dbentrybool(Buf *b, Str key, b32 v)
{
    dbkey(b, key, "b");
    dbbool(b, v);
}

// A whole message around a body built as above, with its signature.
// Null fields are left out.
struct DbHead {
    u8  type;
    u8  flags;
    u32 serial;
    u32 replyto;
    Str path;
    Str iface;
    Str member;
    Str error;
    Str dest;
    Str sig;
    i32 nfds;
};

static void dbfield(Buf *b, u8 code, Str sig, Str v)
{
    dbstruct(b);
    dbu8(b, code);
    dbsig(b, sig);
    if (sig == "g") {
        dbsig(b, v);
    } else {
        dbstr(b, v);
    }
}

static Str dbmessage(Arena *a, DbHead *h, Str body)
{
    Buf b(a, 128 + body.len);
    dbu8(&b, 'l');
    dbu8(&b, h->type);
    dbu8(&b, h->flags);
    dbu8(&b, 1);  // protocol version
    dbu32(&b, (u32)body.len);
    dbu32(&b, h->serial);
    DbArray f = dbarray(&b, 8);
    if (h->path.data)   dbfield(&b, DBF_PATH,   "o", h->path);
    if (h->iface.data)  dbfield(&b, DBF_IFACE,  "s", h->iface);
    if (h->member.data) dbfield(&b, DBF_MEMBER, "s", h->member);
    if (h->error.data)  dbfield(&b, DBF_ERROR,  "s", h->error);
    if (h->dest.data)   dbfield(&b, DBF_DEST,   "s", h->dest);
    if (h->sig.len)     dbfield(&b, DBF_SIG,    "g", h->sig);
    if (h->replyto) {
        dbstruct(&b);
        dbu8(&b, DBF_REPLYTO);
        dbsig(&b, "u");
        dbu32(&b, h->replyto);
    }
    if (h->nfds) {
        dbstruct(&b);
        dbu8(&b, DBF_FDS);
        dbsig(&b, "u");
        dbu32(&b, (u32)h->nfds);
    }
    dbarrayend(&b, f);
    dbpad(&b, 8);
    print(&b, body);
    return finish(&b);
}


// Parsing

struct DbVal {
    DbVal *next;   // the next sibling
    DbVal *child;  // elements, fields, entries' key and value, variant's value
    iz     len;    // children
    u8     type;   // its signature's code: 'a', '(', '{', 'v', 's', ...
    u64    u;      // integers, booleans, fds (an index), as unsigned
    i64    i;      // and signed
    f64    f;
    Str    s;      // strings, paths, signatures; a variant's signature
};

static DbVal const dbmissing = {};

static DbVal *dbnone()
{
    return (DbVal *)&dbmissing;
}

struct DbReader {
    Arena *a;
    Str    data;
    iz     pos;
    b32    ok;
};

static b32 dbalign(DbReader *r, iz align)
{
    iz pad = -r->pos & (align-1);
    if (pad > r->data.len-r->pos) {
        r->ok = 0;
        return 0;
    }
    for (iz i = 0; i < pad; i++) {
        r->ok &= r->data[r->pos+i] == 0;
    }
    r->pos += pad;
    return r->ok;
}

static u64 dbgetn(DbReader *r, iz n)
{
    if (!dbalign(r, n) || n > r->data.len-r->pos) {
        r->ok = 0;
        return 0;
    }
    u64 v = 0;
    for (iz i = n-1; i >= 0; i--) {
        v = v<<8 | r->data[r->pos+i];
    }
    r->pos += n;
    return v;
}

// The end of the single complete type starting at i, or -1.
static iz dbtypeend(Str sig, iz i, i32 depth)
{
    if (i>=sig.len || depth>DB_MAXDEPTH) {
        return -1;
    }
    switch (sig[i]) {
    case 'y': case 'b': case 'n': case 'q': case 'i': case 'u': case 'x':
    case 't': case 'd': case 's': case 'o': case 'g': case 'v': case 'h':
        return i + 1;
    case 'a':
        return dbtypeend(sig, i+1, depth+1);
    case '(': case '{': {
        u8 close = sig[i]=='(' ? ')' : '}';
        iz j     = i + 1;
        while (j < sig.len && sig[j] != close) {
            j = dbtypeend(sig, j, depth+1);
            if (j < 0) {
                return -1;
            }
        }
        return j<sig.len && j>i+1 ? j+1 : -1;
    }
    }
    return -1;
}

static iz dbalignof(u8 c)
{
    switch (c) {
    case 'y': case 'g': case 'v':                     return 1;
    case 'n': case 'q':                               return 2;
    case 'x': case 't': case 'd': case '(': case '{': return 8;
    }
    return 4;
}

static DbVal *dbvalue(DbReader *r, Str sig, i32 depth);

// Values of a run of complete types, as a list.
static DbVal *dbvalues(DbReader *r, Str sig, i32 depth, iz *count)
{
    DbVal  *head = 0;
    DbVal **tail = &head;
    *count = 0;
    for (iz i = 0; r->ok && i < sig.len;) {
        iz end = dbtypeend(sig, i, depth);
        if (end < 0) {
            r->ok = 0;
            break;
        }
        DbVal *v = dbvalue(r, Str{sig.data+i, end-i}, depth);
        *tail = v;
        tail  = &v->next;
        ++*count;
        i = end;
    }
    return head;
}

static DbVal *dbvalue(DbReader *r, Str sig, i32 depth)
{
    DbVal *v = alloc<DbVal>(r->a);
    v->type = sig.len ? sig[0] : 0;
    if (depth > DB_MAXDEPTH) {
        r->ok = 0;
        return v;
    }
    switch (v->type) {
    case 'y': v->u = dbgetn(r, 1); v->i = (i64)v->u;          break;
    case 'n': v->u = dbgetn(r, 2); v->i = (i64)v->u - (v->u>>15)*0x10000; break;
    case 'q': v->u = dbgetn(r, 2); v->i = (i64)v->u;          break;
    case 'b': case 'u': case 'h':
              v->u = dbgetn(r, 4); v->i = (i64)v->u;          break;
    case 'i': v->u = dbgetn(r, 4); v->i = (i32)(u32)v->u;     break;
    case 't': case 'x':
              v->u = dbgetn(r, 8); v->i = (i64)v->u;          break;
    case 'd': {
        v->u = dbgetn(r, 8);
        __builtin_memcpy(&v->f, &v->u, 8);
    } break;
    case 's': case 'o': case 'g': {
        iz n = v->type=='g' ? (iz)dbgetn(r, 1) : (iz)dbgetn(r, 4);
        if (!r->ok || n >= r->data.len-r->pos || r->data[r->pos+n]) {
            r->ok = 0;
            break;
        }
        v->s    = Str{r->data.data+r->pos, n};
        r->pos += n + 1;
    } break;
    case 'v': {
        iz n = (iz)dbgetn(r, 1);
        if (!r->ok || n >= r->data.len-r->pos) {
            r->ok = 0;
            break;
        }
        v->s    = Str{r->data.data+r->pos, n};
        r->pos += n + 1;
        if (dbtypeend(v->s, 0, depth+1) != n) {
            r->ok = 0;
            break;
        }
        v->child = dbvalue(r, v->s, depth+1);
        v->len   = 1;
    } break;
    case 'a': {
        Str elem = cuthead(sig, 1);
        iz  n    = (iz)dbgetn(r, 4);
        dbalign(r, dbalignof(elem.len ? elem[0] : 0));
        if (!r->ok || n > r->data.len-r->pos || n > (iz)1<<26) {
            r->ok = 0;
            break;
        }
        iz      end  = r->pos + n;
        DbVal **tail = &v->child;
        while (r->ok && r->pos < end) {
            DbVal *e = dbvalue(r, elem, depth+1);
            *tail = e;
            tail  = &e->next;
            v->len++;
        }
        r->ok &= r->pos == end;
    } break;
    case '(': case '{':
        dbalign(r, 8);
        v->child = dbvalues(r, Str{sig.data+1, sig.len-2}, depth+1, &v->len);
        r->ok &= v->type!='{' || v->len==2;
        break;
    default:
        r->ok = 0;
    }
    return v;
}

// A body's values as the children of one struct-like value, or null if
// the body does not match its signature.
static DbVal *dbparse(Arena *a, Str body, Str sig)
{
    DbReader r = {};
    r.a    = a;
    r.data = body;
    r.ok   = 1;
    DbVal *v = alloc<DbVal>(a);
    v->type  = '(';
    v->child = dbvalues(&r, sig, 0, &v->len);
    return r.ok && r.pos==body.len ? v : 0;
}

// The i-th child: an element, a field, an argument.
static DbVal *dbat(DbVal *v, iz i)
{
    DbVal *c = v->child;
    for (; c && i > 0; i--) {
        c = c->next;
    }
    return c && i==0 ? c : dbnone();
}

// Through variants to what they hold.
static DbVal *dbunwrap(DbVal *v)
{
    for (i32 i = 0; v->type=='v' && i < DB_MAXDEPTH; i++) {
        v = v->child ? v->child : dbnone();
    }
    return v;
}

// The value of key in an a{s...} dictionary, unwrapped from its variant.
static DbVal *dbget(DbVal *dict, Str key)
{
    dict = dbunwrap(dict);
    if (dict->type != 'a') {
        return dbnone();
    }
    for (DbVal *e = dict->child; e; e = e->next) {
        if (e->type=='{' && e->child && e->child->s==key) {
            return dbunwrap(dbat(e, 1));
        }
    }
    return dbnone();
}


// Messages read back

struct DbMsg {
    DbHead h;
    Str    sender;
    Str    body;
    iz     len;   // the whole message's bytes
    b32    ok;
};

// The size of the message at the start of buf once its fixed header is
// there: 0 if more is needed to say, -1 if it is no message.
static iz dbmsgsize(Str buf)
{
    if (buf.len < 16) {
        return 0;
    } else if (buf[0]!='l' || buf[3]!=1) {
        return -1;  // big-endian peers are not expected on a local bus
    }
    u32 body = (u32)buf[4] | (u32)buf[5]<<8 | (u32)buf[6]<<16 | (u32)buf[7]<<24;
    u32 hdr  = (u32)buf[12] | (u32)buf[13]<<8 | (u32)buf[14]<<16 | (u32)buf[15]<<24;
    if (body>DB_MSGMAX || hdr>DB_MSGMAX) {
        return -1;
    }
    iz n = 16 + (iz)hdr;
    n += -n & 7;
    n += body;
    return n<=DB_MSGMAX ? n : -1;
}

// One whole message, as dbmsgsize measured it.
static DbMsg dbreadmsg(Arena *a, Str m)
{
    DbMsg r = {};
    iz    n = dbmsgsize(m);
    if (n<=0 || n>m.len) {
        return r;
    }
    r.len       = n;
    r.h.type    = m[1];
    r.h.flags   = m[2];
    DbReader rd = {};
    rd.a    = a;
    rd.data = Str{m.data, n};
    rd.pos  = 4;
    rd.ok   = 1;
    iz bodylen = (iz)dbgetn(&rd, 4);
    r.h.serial = (u32)dbgetn(&rd, 4);
    DbVal *f = dbvalue(&rd, "a(yv)", 0);
    dbalign(&rd, 8);
    if (!rd.ok || rd.pos+bodylen!=n) {
        return r;
    }
    for (DbVal *e = f->child; e; e = e->next) {
        DbVal *v = dbunwrap(dbat(e, 1));
        switch (dbat(e, 0)->u) {
        case DBF_PATH:    r.h.path    = v->s;      break;
        case DBF_IFACE:   r.h.iface   = v->s;      break;
        case DBF_MEMBER:  r.h.member  = v->s;      break;
        case DBF_ERROR:   r.h.error   = v->s;      break;
        case DBF_REPLYTO: r.h.replyto = (u32)v->u; break;
        case DBF_DEST:    r.h.dest    = v->s;      break;
        case DBF_SENDER:  r.sender    = v->s;      break;
        case DBF_SIG:     r.h.sig     = v->s;      break;
        case DBF_FDS:     r.h.nfds    = (i32)v->u; break;
        }
    }
    r.body = Str{m.data+rd.pos, bodylen};
    r.ok   = r.h.type>=DB_CALL && r.h.type<=DB_SIGNAL;
    return r;
}

// A portal's request path from its handle token, predicted as the
// specification says: the sender's unique name without its colon, with
// its dots as underscores.
static Str dbrequestpath(Arena *a, Str unique, Str token)
{
    Buf b(a, 64);
    print(&b, "/org/freedesktop/portal/desktop/request/");
    for (iz i = unique.len && unique[0]==':'; i < unique.len; i++) {
        putbyte(&b, unique[i]=='.' ? '_' : unique[i]);
    }
    putbyte(&b, '/');
    print(&b, token);
    return finish(&b);
}
