// Zip tests: members stored and deflated (by png.cpp's encoder, as
// Wuffs then inflates them), found by name past others and a comment,
// and refused when missing, cut short, encrypted, of another method,
// or not what their CRC says.

struct ZipTestMember {
    Str name;
    Str data;
    b32 deflate;
};

static void zipput16(Buf *b, u32 v)
{
    u8 p[2] = {(u8)v, (u8)(v>>8)};
    print(b, Str(p, 2));
}

static void zipput32(Buf *b, u32 v)
{
    u8 p[4] = {(u8)v, (u8)(v>>8), (u8)(v>>16), (u8)(v>>24)};
    print(b, Str(p, 4));
}

// A zip of the members as engines.py writes one, with a comment; flags
// set on every entry (central and local)
static Str ziptest(Arena *a, ZipTestMember *ms, iz n, u32 flags = 0)
{
    Buf  b(a, 1<<12);
    iz  *offs  = alloc<iz>(a, n);
    Str *datas = alloc<Str>(a, n);
    for (iz i = 0; i < n; i++) {
        Str d = ms[i].data;
        if (ms[i].deflate) {
            iz  cap = d.len*9/8 + 16;
            u8 *out = alloc<u8>(a, cap);
            d = Str(out, zdeflate(*a, out, cap, d.data, d.len));
        }
        datas[i] = d;
        offs[i]  = b.len;
        zipput32(&b, 0x04034b50);
        zipput16(&b, 20);
        zipput16(&b, flags);
        zipput16(&b, ms[i].deflate ? 8 : 0);
        zipput32(&b, 0);  // time and date
        zipput32(&b, zcrc32(0, ms[i].data.data, ms[i].data.len));
        zipput32(&b, (u32)d.len);
        zipput32(&b, (u32)ms[i].data.len);
        zipput16(&b, (u32)ms[i].name.len);
        zipput16(&b, 0);
        print(&b, ms[i].name);
        print(&b, d);
    }
    iz cd = b.len;
    for (iz i = 0; i < n; i++) {
        zipput32(&b, 0x02014b50);
        zipput16(&b, 0x0314);
        zipput16(&b, 20);
        zipput16(&b, flags);
        zipput16(&b, ms[i].deflate ? 8 : 0);
        zipput32(&b, 0);
        zipput32(&b, zcrc32(0, ms[i].data.data, ms[i].data.len));
        zipput32(&b, (u32)datas[i].len);
        zipput32(&b, (u32)ms[i].data.len);
        zipput16(&b, (u32)ms[i].name.len);
        zipput16(&b, 0);
        zipput16(&b, 0);
        zipput16(&b, 0);
        zipput16(&b, 0);
        zipput32(&b, 0100755u << 16);
        zipput32(&b, (u32)offs[i]);
        print(&b, ms[i].name);
    }
    Str comment = "a comment";
    zipput32(&b, 0x06054b50);
    zipput16(&b, 0);
    zipput16(&b, 0);
    zipput16(&b, (u32)n);
    zipput16(&b, (u32)n);
    zipput32(&b, (u32)(b.len - 12 - cd));  // the central directory's length
    zipput32(&b, (u32)cd);
    zipput16(&b, (u32)comment.len);
    print(&b, comment);
    return finish(&b);
}

static Str zipgot(Arena *a, Str zip, Str name)
{
    ZipRead r = zipread(a, zip, name, 1<<20);
    return r.err.len ? concat(a, "error: ", r.err) : r.data;
}

static void test_zip(Test *t)
{
    Arena a = t->perm;

    // A run of text that deflates well, and bytes of every value
    Buf big(&a, 1<<14);
    for (i32 i = 0; i < 400; i++) {
        print(&big, "prospero engine ");
        print(&big, (i64)(i % 7));
    }
    Str text = finish(&big);
    u8 *bin  = alloc<u8>(&a, 1000);
    for (i32 i = 0; i < 1000; i++) {
        bin[i] = (u8)(i*131 + 7);
    }
    ZipTestMember ms[] = {
        {"top/README.txt", "Prospero engines\n", 0},
        {"top/engines/prospero-engine-0.0.0-linux-x86_64", text, 1},
        {"top/engines/prospero-engine-0.0.0-linux-x86_64.sha256", "abc  x\n", 0},
        {"top/engines/bin", Str(bin, 1000), 1},
        {"top/engines/empty", "", 1},
    };
    Str zip = ziptest(&a, ms, countof(ms));

    for (iz i = 0; i < countof(ms); i++) {
        expect(t, concat(&a, "zip.read ", ms[i].name), zipgot(&a, zip, ms[i].name), ms[i].data);
    }
    ZipRead r = zipread(&a, zip, "top/engines/none", 1<<20);
    expect(t, "zip.missing", istr(&a, r.missing), "1");
    r = zipread(&a, zip, "top/engines", 1<<20);
    expect(t, "zip.prefix", istr(&a, r.missing), "1");
    r = zipread(&a, zip, ms[1].name, 100);
    expect(t, "zip.max", r.err, concat(&a, ms[1].name, ": too large"));

    // Cut short anywhere, it reads nothing: the end record goes first
    b32 any = 0;
    for (iz n = 0; n < zip.len; n += 7) {
        ZipRead c = zipread(&a, Str(zip.data, n), ms[1].name, 1<<20);
        any |= c.data.len != 0;
    }
    expect(t, "zip.cut", istr(&a, any), "0");

    // A flipped byte in the deflated data: its CRC or its inflate says so
    Str bad = clone(&a, zip);
    ZipMember m = zipfind(bad, ms[1].name);
    m.data.data[m.data.len/2] ^= 0x40;
    r = zipread(&a, bad, ms[1].name, 1<<20);
    expect(t, "zip.corrupt", istr(&a, r.err.len>0 && !r.missing), "1");
    // In stored data, the CRC alone
    bad = clone(&a, zip);
    m   = zipfind(bad, ms[0].name);
    m.data.data[0] ^= 1;
    r = zipread(&a, bad, ms[0].name, 1<<20);
    expect(t, "zip.crc", r.err, concat(&a, ms[0].name, ": CRC mismatch"));

    // Encrypted entries are refused
    Str enc = ziptest(&a, ms, 1, 1);
    r = zipread(&a, enc, ms[0].name, 1<<20);
    expect(t, "zip.encrypted", istr(&a, r.missing), "1");

    // Inflate: exactly the size, neither more nor less
    iz    n     = wuffsinflatestate();
    u8   *mem   = alloc<u8>(&a, n+15);
    void *state = (void *)(((uz)mem + 15) & ~(uz)15);
    m = zipfind(zip, ms[1].name);
    u8 *out = alloc<u8>(&a, text.len+2);
    expect(t, "zip.inflate.exact",
           wuffsinflate(state, out, text.len, m.data.data, m.data.len) ? Str("err") : Str("ok"), "ok");
    expect(t, "zip.inflate.data", Str(out, text.len), text);
    // Each with fresh, zeroed state
    state = (void *)(((uz)alloc<u8>(&a, n+15) + 15) & ~(uz)15);
    expect(t, "zip.inflate.long",
           wuffsinflate(state, out, text.len-1, m.data.data, m.data.len) ? Str("err") : Str("ok"), "err");
    state = (void *)(((uz)alloc<u8>(&a, n+15) + 15) & ~(uz)15);
    expect(t, "zip.inflate.short",
           wuffsinflate(state, out, text.len+1, m.data.data, m.data.len) ? Str("err") : Str("ok"), "err");
}
