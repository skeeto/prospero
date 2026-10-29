// wuffs: image decoding through Google's Wuffs, a translation unit of
// its own, in C, so that Wuffs and its system headers stay out of the
// core, which reaches it through these three functions (image.cpp)
//
// Wuffs (github.com/google/wuffs, Apache-2.0 OR MIT) is fetched as its
// single-file release, wuffs-v0.4.c, which CMake puts on the include
// path. Only the decoders the core takes are compiled: PNG, JPEG, GIF,
// BMP and WebP, with what they need. Every function is static but these,
// and all memory comes from the caller, sized by wuffsstate and by the
// header's worklen: Wuffs itself never allocates.
//   $ cc -c -O2 -I build/_deps/wuffs-src src/wuffs.c
//
// This is free and unencumbered software released into the public domain.
#define WUFFS_IMPLEMENTATION
#define WUFFS_CONFIG__STATIC_FUNCTIONS
#define WUFFS_CONFIG__MODULES
#define WUFFS_CONFIG__MODULE__BASE
#define WUFFS_CONFIG__MODULE__ADLER32
#define WUFFS_CONFIG__MODULE__BMP
#define WUFFS_CONFIG__MODULE__CRC32
#define WUFFS_CONFIG__MODULE__DEFLATE
#define WUFFS_CONFIG__MODULE__GIF
#define WUFFS_CONFIG__MODULE__JPEG
#define WUFFS_CONFIG__MODULE__LZW
#define WUFFS_CONFIG__MODULE__PNG
#define WUFFS_CONFIG__MODULE__VP8
#define WUFFS_CONFIG__MODULE__WEBP
#define WUFFS_CONFIG__MODULE__ZLIB
#define WUFFS_CONFIG__DST_PIXEL_FORMAT__ENABLE_ALLOWLIST
#define WUFFS_CONFIG__DST_PIXEL_FORMAT__ALLOW_RGBA_NONPREMUL
#include <stddef.h>
#include <stdlib.h>
// Wuffs' allocating constructors, which are never called, name calloc
// and free, which the CRT-free Windows engine has not: nothing then
#define calloc(n, size) ((void *)0)
#define free(p)         ((void)(p))
#include "wuffs-v0.4.c"

// Kinds, as image.cpp numbers them
enum {
    WK_PNG = 1,
    WK_JPEG,
    WK_GIF,
    WK_BMP,
    WK_WEBP,
};

typedef struct {
    int32_t     width;
    int32_t     height;
    ptrdiff_t   worklen;  // bytes of work buffer the decode needs
    char const *err;      // Wuffs' status message, null if none
} WuffsHead;

// What a decode keeps between its header and its pixels, at the start
// of the caller's state, the decoder after it. Each decoder is called
// directly, by kind, rather than through Wuffs' vtables, whose calls
// through pointers of another type UBSan reports (-fsanitize=function).
typedef struct {
    wuffs_base__io_buffer src;
    int32_t               kind;
    uint32_t              width;
    uint32_t              height;
} State;

enum { STATEHEAD = (sizeof(State) + 15) & ~(size_t)15 };

// Bytes of state a decode of this kind needs, zeroed and 16-byte
// aligned, or 0 for an unknown kind.
ptrdiff_t wuffsstate(int32_t kind)
{
    size_t n = 0;
    switch (kind) {
    case WK_PNG:  n = sizeof__wuffs_png__decoder();  break;
    case WK_JPEG: n = sizeof__wuffs_jpeg__decoder(); break;
    case WK_GIF:  n = sizeof__wuffs_gif__decoder();  break;
    case WK_BMP:  n = sizeof__wuffs_bmp__decoder();  break;
    case WK_WEBP: n = sizeof__wuffs_webp__decoder(); break;
    }
    return n ? (ptrdiff_t)(STATEHEAD + n) : 0;
}

static char const *errof(wuffs_base__status s)
{
    return s.repr ? s.repr : "#wuffs: unknown error";
}

static wuffs_base__status initialize(int32_t kind, void *d, size_t n)
{
    uint32_t z = WUFFS_INITIALIZE__ALREADY_ZEROED;
    switch (kind) {
    case WK_PNG:  return wuffs_png__decoder__initialize(d, n, WUFFS_VERSION, z);
    case WK_JPEG: return wuffs_jpeg__decoder__initialize(d, n, WUFFS_VERSION, z);
    case WK_GIF:  return wuffs_gif__decoder__initialize(d, n, WUFFS_VERSION, z);
    case WK_BMP:  return wuffs_bmp__decoder__initialize(d, n, WUFFS_VERSION, z);
    case WK_WEBP: return wuffs_webp__decoder__initialize(d, n, WUFFS_VERSION, z);
    }
    return wuffs_base__make_status("#wuffs: unknown kind");
}

static wuffs_base__status imageconfig(State *s, void *d, wuffs_base__image_config *ic)
{
    wuffs_base__io_buffer *src = &s->src;
    switch (s->kind) {
    case WK_PNG:  return wuffs_png__decoder__decode_image_config(d, ic, src);
    case WK_JPEG: return wuffs_jpeg__decoder__decode_image_config(d, ic, src);
    case WK_GIF:  return wuffs_gif__decoder__decode_image_config(d, ic, src);
    case WK_BMP:  return wuffs_bmp__decoder__decode_image_config(d, ic, src);
    case WK_WEBP: return wuffs_webp__decoder__decode_image_config(d, ic, src);
    }
    return wuffs_base__make_status("#wuffs: unknown kind");
}

static uint64_t workbuflen(State *s, void *d)
{
    switch (s->kind) {
    case WK_PNG:  return wuffs_png__decoder__workbuf_len(d).max_incl;
    case WK_JPEG: return wuffs_jpeg__decoder__workbuf_len(d).max_incl;
    case WK_GIF:  return wuffs_gif__decoder__workbuf_len(d).max_incl;
    case WK_BMP:  return wuffs_bmp__decoder__workbuf_len(d).max_incl;
    case WK_WEBP: return wuffs_webp__decoder__workbuf_len(d).max_incl;
    }
    return 0;
}

static wuffs_base__status frame(State *s, void *d, wuffs_base__pixel_buffer *pb,
                                wuffs_base__slice_u8 work)
{
    wuffs_base__io_buffer  *src   = &s->src;
    wuffs_base__pixel_blend blend = WUFFS_BASE__PIXEL_BLEND__SRC;
    switch (s->kind) {
    case WK_PNG:  return wuffs_png__decoder__decode_frame(d, pb, src, blend, work, 0);
    case WK_JPEG: return wuffs_jpeg__decoder__decode_frame(d, pb, src, blend, work, 0);
    case WK_GIF:  return wuffs_gif__decoder__decode_frame(d, pb, src, blend, work, 0);
    case WK_BMP:  return wuffs_bmp__decoder__decode_frame(d, pb, src, blend, work, 0);
    case WK_WEBP: return wuffs_webp__decoder__decode_frame(d, pb, src, blend, work, 0);
    }
    return wuffs_base__make_status("#wuffs: unknown kind");
}

// Start a decode of the image in data (kept until wuffsdecode), in
// state, zeroed, of wuffsstate(kind) bytes: its size, and its work
// buffer's, from its header.
WuffsHead wuffshead(void *state, int32_t kind, uint8_t *data, ptrdiff_t len)
{
    WuffsHead r = {0};
    State    *s = state;
    void     *d = (char *)state + STATEHEAD;
    ptrdiff_t n = wuffsstate(kind);
    if (!n) {
        r.err = "#wuffs: unknown kind";
        return r;
    }
    wuffs_base__status st = initialize(kind, d, (size_t)n - STATEHEAD);
    if (!wuffs_base__status__is_ok(&st)) {
        r.err = errof(st);
        return r;
    }

    s->kind = kind;
    s->src  = wuffs_base__ptr_u8__reader(data, (size_t)len, 1);
    wuffs_base__image_config ic = {0};
    st = imageconfig(s, d, &ic);
    if (!wuffs_base__status__is_ok(&st)) {
        r.err = errof(st);
        return r;
    }
    s->width  = wuffs_base__pixel_config__width(&ic.pixcfg);
    s->height = wuffs_base__pixel_config__height(&ic.pixcfg);
    uint64_t work = workbuflen(s, d);
    if (s->width>0x7fffffff || s->height>0x7fffffff || work>PTRDIFF_MAX/2) {
        r.err = "#wuffs: too large";
        return r;
    }
    r.width   = (int32_t)s->width;
    r.height  = (int32_t)s->height;
    r.worklen = (ptrdiff_t)work;
    return r;
}

// Decode the first frame of the image wuffshead started into rgba,
// width*height pixels, RGBA, not premultiplied, top row first, zeroed
// where the frame does not cover it. Returns Wuffs' error, null if none.
char const *wuffsdecode(void *state, uint8_t *rgba, uint8_t *work, ptrdiff_t worklen)
{
    State *s = state;
    void  *d = (char *)state + STATEHEAD;
    wuffs_base__pixel_config pc = {0};
    wuffs_base__pixel_config__set(
        &pc, WUFFS_BASE__PIXEL_FORMAT__RGBA_NONPREMUL,
        WUFFS_BASE__PIXEL_SUBSAMPLING__NONE, s->width, s->height
    );
    wuffs_base__pixel_buffer pb = {0};
    uint64_t len = (uint64_t)s->width * s->height * 4;
    wuffs_base__status st = wuffs_base__pixel_buffer__set_from_slice(
        &pb, &pc, wuffs_base__make_slice_u8(rgba, (size_t)len)
    );
    if (!wuffs_base__status__is_ok(&st)) {
        return errof(st);
    }
    st = frame(s, d, &pb, wuffs_base__make_slice_u8(work, (size_t)worklen));
    return st.repr ? errof(st) : 0;
}

// Bytes of zeroed, 16-byte aligned state for wuffsinflate.
ptrdiff_t wuffsinflatestate(void)
{
    return (ptrdiff_t)sizeof__wuffs_deflate__decoder();
}

// Inflate raw DEFLATE (RFC 1951) from src into dst, which it must fill
// to exactly dstlen bytes, in state, zeroed, of wuffsinflatestate()
// bytes. dst has room for one byte more, so that a stream longer than
// dstlen is told from one that ends just there. Returns Wuffs' error, or
// one of its own, null if none.
char const *wuffsinflate(void *state, uint8_t *dst, ptrdiff_t dstlen,
                         uint8_t *src, ptrdiff_t srclen)
{
    wuffs_base__status st = wuffs_deflate__decoder__initialize(
        state, (size_t)wuffsinflatestate(), WUFFS_VERSION,
        WUFFS_INITIALIZE__ALREADY_ZEROED
    );
    if (!wuffs_base__status__is_ok(&st)) {
        return errof(st);
    }
    wuffs_base__io_buffer in  = wuffs_base__ptr_u8__reader(src, (size_t)srclen, 1);
    wuffs_base__io_buffer out = wuffs_base__ptr_u8__writer(dst, (size_t)dstlen + 1);
    uint8_t work[WUFFS_DEFLATE__DECODER_WORKBUF_LEN_MAX_INCL_WORST_CASE + 1];
    st = wuffs_deflate__decoder__transform_io(
        state, &out, &in, wuffs_base__make_slice_u8(work, sizeof(work))
    );
    if (st.repr == wuffs_base__suspension__short_write) {
        return "#wuffs: inflates to more than expected";
    } else if (st.repr == wuffs_base__suspension__short_read) {
        return "#wuffs: cut short";
    } else if (!wuffs_base__status__is_ok(&st)) {
        return errof(st);
    } else if (out.meta.wi != (size_t)dstlen) {
        return "#wuffs: inflates to less than expected";
    }
    return 0;
}
