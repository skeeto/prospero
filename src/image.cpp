// Images: what one is, by its first bytes and its header, and its
// pixels, decoded by Wuffs (wuffs.c, a translation unit of its own, in
// C): PNG, interlaced or not, JPEG, baseline or progressive, GIF (its
// first frame), BMP and WebP, lossless or lossy, into RGBA
//
// The input is untrusted: from a session's directory, maybe on another
// machine, or uploaded, or pasted. Wuffs is memory-safe by construction,
// and never allocates: its decoder's state, the pixels and its work
// buffer come from the arena, sized from the header and checked against
// the caller's limit, and the arena's room, before any are allocated,
// so that a hostile header cannot exhaust memory. A bad image fails with
// a reason.
//
// This is free and unencumbered software released into the public domain.

struct WuffsHead {
    i32         width;
    i32         height;
    iz          worklen;  // bytes of work buffer the decode needs
    char const *err;      // Wuffs' status message, null if none
};

// The bridge to Wuffs (wuffs.c)
extern "C" {
    // Bytes of zeroed, 16-byte aligned state for a decode of the kind,
    // 0 if unknown.
    iz          wuffsstate(i32 kind);
    // Parse the header of an image, its data kept until the decode.
    WuffsHead   wuffshead(void *state, i32 kind, u8 *data, iz len);
    // Decode the first frame into width*height RGBA pixels, zeroed where
    // it does not cover them. Wuffs' error, or null.
    char const *wuffsdecode(void *state, u8 *rgba, u8 *work, iz worklen);
}

enum : i32 {
    IMAGE_NONE,
    IMAGE_PNG,
    IMAGE_JPEG,
    IMAGE_GIF,
    IMAGE_BMP,
    IMAGE_WEBP,

    IMAGE_EDGE = 65535,  // pixels on a side, at most
};

static struct { Str mime; Str ext; Str name; } const imagetypes[] = {
    {},
    {"image/png",  "png",  "PNG"},
    {"image/jpeg", "jpg",  "JPEG"},
    {"image/gif",  "gif",  "GIF"},
    {"image/bmp",  "bmp",  "BMP"},
    {"image/webp", "webp", "WebP"},
};

// The kind of image by its first bytes, IMAGE_NONE if none taken.
static i32 imagesniff(Str d)
{
    if (startswith(d, "\x89PNG\r\n\x1a\n")) {
        return IMAGE_PNG;
    } else if (d.len>=3 && d[0]==0xff && d[1]==0xd8 && d[2]==0xff) {
        return IMAGE_JPEG;
    } else if (startswith(d, "GIF87a") || startswith(d, "GIF89a")) {
        return IMAGE_GIF;
    } else if (startswith(d, "RIFF") && cuthead(takehead(d, 12), 8)=="WEBP") {
        return IMAGE_WEBP;
    } else if (startswith(d, "BM")) {
        return IMAGE_BMP;
    }
    return IMAGE_NONE;
}

// A type's short name, PNG or JPEG and so on, by its MIME type.
static Str imagename(Str mime)
{
    for (i32 i = 1; i < countof(imagetypes); i++) {
        if (mime == imagetypes[i].mime) {
            return imagetypes[i].name;
        }
    }
    return "image";
}

// Wuffs' message, less its leading # (error), $ (suspension) or @ (note).
static Str wuffserr(char const *e)
{
    Str s = {(u8 *)e, 0};
    for (; s.len<256 && e[s.len]; s.len++) {}  // bounded: no strlen
    s = cuthead(s, 1);
    if (endswith(s, ": truncated input") || s=="base: short read") {
        return "it ends early";
    }
    return s;
}

struct alignas(16) WuffsBlock {
    u8 b[16];
};

struct ImageHead {
    void *state;  // for the decode to follow
    i32   width;
    i32   height;
    iz    worklen;
    Str   err;
};

// Parse an image's header, its decoder's state allocated in a, once its
// room there is checked.
static ImageHead imagehead(Arena *a, i32 kind, Str data)
{
    ImageHead r = {};
    iz        n = wuffsstate(kind);
    if (!n) {
        r.err = "an unknown kind of image";
        return r;
    } else if (!fits(a, n + 16)) {
        r.err = "out of memory to read it";
        return r;
    }
    r.state = alloc<WuffsBlock>(a, (n + 15)/16);
    WuffsHead h = wuffshead(r.state, kind, data.data, data.len);
    if (h.err) {
        r.err = wuffserr(h.err);
        return r;
    } else if (h.width<1 || h.height<1) {
        r.err = "a bad size";
        return r;
    }
    r.width   = h.width;
    r.height  = h.height;
    r.worklen = h.worklen;
    return r;
}

// What an image to attach is, by its first bytes: one of the kinds
// taken, else none (null mime), with its size from its header, 0 by 0
// if its header is bad, and why.
struct ImageKind {
    Str mime;
    Str ext;   // png, jpg, gif, bmp or webp
    Str name;  // PNG, JPEG, GIF, BMP or WebP
    i32 kind;
    i32 w, h;
    Str err;
};

static ImageKind imagekind(Arena scratch, Str data)
{
    ImageKind r = {};
    r.kind = imagesniff(data);
    if (!r.kind) {
        return r;
    }
    r.mime = imagetypes[r.kind].mime;
    r.ext  = imagetypes[r.kind].ext;
    r.name = imagetypes[r.kind].name;
    ImageHead h = imagehead(&scratch, r.kind, data);
    if (h.err.data) {
        r.err = h.err;
        return r;
    }
    r.w = h.width;
    r.h = h.height;
    return r;
}

struct Image {
    u8  *rgba;    // width*height pixels, top row first, null on error
    i32  width;
    i32  height;
    Str  err;     // why it failed, null if it did not
};

// Decode an image of at most maxpixels pixels into RGBA, not
// premultiplied. The pixels go in a, which is left as it was on
// failure, and the decoder's temporaries after them, for the caller to
// discard with the arena: the work buffer, at most 16 bytes a pixel.
static Image imgdecode(Arena *a, Str data, i64 maxpixels)
{
    Image r    = {};
    i32   kind = imagesniff(data);
    if (!kind) {
        r.err = "not a PNG, JPEG, GIF, WebP or BMP image";
        return r;
    }

    // The header, parsed once in scratch to size the pixels, which go
    // first, and again in the scratch after them for the decode
    Arena     t = *a;
    ImageHead h = imagehead(&t, kind, data);
    if (h.err.data) {
        r.err = h.err;
        return r;
    }
    i64 w = h.width;
    i64 n = w * h.height;
    if (h.width>IMAGE_EDGE || h.height>IMAGE_EDGE || n>maxpixels) {
        r.err = "too many pixels";
        return r;
    } else if (h.worklen > 16*n + ((iz)1<<20)) {
        r.err = "too much memory to decode it";
        return r;
    }
    i64 need = 4*n + wuffsstate(kind) + h.worklen + 64;
    if (need>(i64)((uz)-1 >> 1) || !fits(a, (iz)need)) {
        r.err = "out of memory to decode it";
        return r;
    }

    Arena save = *a;
    u8   *rgba = alloc<u8>(a, (iz)(4*n));  // zero where a frame does not reach
    t = *a;
    h = imagehead(&t, kind, data);
    assert(!h.err.data && h.width==w && h.height*w==n);
    u8         *work = allocbytes(&t, h.worklen);
    char const *err  = wuffsdecode(h.state, rgba, work, h.worklen);
    if (err) {
        *a    = save;
        r.err = wuffserr(err);
        return r;
    }
    r.rgba   = rgba;
    r.width  = h.width;
    r.height = h.height;
    return r;
}
