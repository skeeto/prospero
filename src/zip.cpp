// Zip: one member of an archive in memory, found by name and extracted,
// as tools/engines.py writes them (stored or deflated, no zip64, no
// encryption), for the engines an itch.io build embeds (deploy.cpp).
// Every offset and size is checked against the archive, which need not
// be trusted, though the embedded one is.

static u32 zipget16(u8 *p)
{
    return (u32)p[0] | (u32)p[1]<<8;
}

static u32 zipget32(u8 *p)
{
    return (u32)p[0] | (u32)p[1]<<8 | (u32)p[2]<<16 | (u32)p[3]<<24;
}

struct ZipMember {
    Str data;    // as stored: compressed for method 8
    i64 size;    // extracted
    u32 crc;
    u32 method;  // 0 stored, 8 deflated
    b32 ok;      // found, and sound
};

// The member named name, from the central directory, its data where its
// local header says. Not ok if it is missing, or the archive is not
// sound where the search goes. Offsets and sizes are i64, as the
// archive's are 32 bits unsigned and iz is 32 bits signed on i686.
static ZipMember zipfind(Str zip, Str name)
{
    ZipMember r   = {};
    i64       len = zip.len;
    // The end of central directory record: 22 bytes, then a comment of
    // up to 65535, so it is searched for back from the end
    i64 eocd = -1;
    for (i64 i = len-22; i>=0 && i>=len-22-65535; i--) {
        if (zipget32(zip.data+i) == 0x06054b50) {
            eocd = i;
            break;
        }
    }
    if (eocd < 0) {
        return r;
    }
    u8 *e     = zip.data + eocd;
    i64 count = zipget16(e+10);
    i64 cdlen = zipget32(e+12);
    i64 cdoff = zipget32(e+16);
    if (zipget16(e+20) > len-eocd-22) {
        return r;  // its comment cut short
    } else if (cdoff>eocd || cdlen>eocd-cdoff) {
        return r;
    }
    i64 at = cdoff;
    for (i64 n = 0; n < count; n++) {
        if (cdoff+cdlen-at < 46) {
            return r;
        }
        u8 *c = zip.data + at;
        if (zipget32(c) != 0x02014b50) {
            return r;
        }
        i64 namelen = zipget16(c+28);
        i64 skip    = namelen + zipget16(c+30) + zipget16(c+32);
        if (cdoff+cdlen-at-46 < skip) {
            return r;
        }
        if (Str(c+46, (iz)namelen) != name) {
            at += 46 + skip;
            continue;
        }

        u32 flags  = zipget16(c+8);
        u32 method = zipget16(c+10);
        i64 clen   = zipget32(c+20);
        i64 ulen   = zipget32(c+24);
        i64 local  = zipget32(c+42);
        if (flags&1 || (method!=0 && method!=8) || clen==0xffffffff ||
                ulen==0xffffffff || local==0xffffffff) {
            return r;  // encrypted, another method, or zip64
        }
        if (local>cdoff || cdoff-local<30) {
            return r;
        }
        u8 *l = zip.data + local;
        if (zipget32(l) != 0x04034b50) {
            return r;
        }
        i64 data = local + 30 + zipget16(l+26) + zipget16(l+28);
        if (data>cdoff || clen>cdoff-data) {
            return r;
        }
        if (method==0 && clen!=ulen) {
            return r;
        }
        r.data   = Str(zip.data+data, (iz)clen);  // inside the archive
        r.size   = ulen;
        r.crc    = zipget32(c+16);
        r.method = method;
        r.ok     = 1;
        return r;
    }
    return r;
}

// A member extracted, or why not: missing (or its entry unsound), or
// why it could not be extracted.
struct ZipRead {
    Str data;
    Str err;
    b32 missing;
};

// The member named name, extracted into a, its CRC checked, at most max
// bytes. The inflate's state comes from a too: it is small.
static ZipRead zipread(Arena *a, Str zip, Str name, iz max)
{
    ZipRead   r = {};
    ZipMember m = zipfind(zip, name);
    if (!m.ok) {
        r.missing = 1;
        r.err     = concat(a, name, ": not in the archive, or unsound");
        return r;
    }
    if (m.size > max) {
        r.err = concat(a, name, ": too large");
        return r;
    }
    if (m.method == 0) {
        r.data = clone(a, m.data);
    } else {
        iz    n     = wuffsinflatestate();
        u8   *mem   = alloc<u8>(a, n+15);
        void *state = (void *)(((uz)mem + 15) & ~(uz)15);
        iz    size  = (iz)m.size;  // at most max
        u8   *out   = alloc<u8>(a, size+1);
        char const *e = wuffsinflate(state, out, size, m.data.data, m.data.len);
        if (e) {
            Str why = concat(a, ": ", wuffserr(e));
            r.err = concat(a, name, why);
            return r;
        }
        r.data = Str(out, size);
    }
    if (zcrc32(0, r.data.data, r.data.len) != m.crc) {
        r.data = {};
        r.err  = concat(a, name, ": CRC mismatch");
        return r;
    }
    return r;
}
