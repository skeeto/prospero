// Frames: a reliable, reconnectable stream of protocol lines
//
// A host (host.cpp) and its clients (remote.cpp) exchange protocol v0
// lines over a byte stream that may break at any moment and be replaced
// by another: ssh's standard I/O, bridged by "prospero attach" to the
// host's local endpoint, or that endpoint itself. The framing, after VS
// Code's PersistentProtocol, makes the replacement seamless: nothing is
// lost, nothing arrives twice, and the order holds.
//
// A frame is a 13-byte header, all big-endian, then its payload:
//
//   u8  type   REGULAR 1, ACK 3, DISCONNECT 5, REPLAY 6, KEEPALIVE 9
//   u32 id     a regular frame's number, from 1 in each direction
//   u32 ack    the last regular frame the sender received in order
//   u32 len    payload bytes: a regular frame's at most 8 MiB
//
// A regular frame carries one protocol line. Every frame acknowledges,
// and a sender keeps each regular frame until the peer acknowledges it,
// up to a count and a size: past those it takes no more (linksend), and
// the layer above catches up by other means, open{sid,from} replays
// from the session logs. A receiver acknowledges at least every 2 s or
// 50 frames, a sender with nothing to say sends a keepalive every 5 s,
// and a stream that brings no bytes for 20 s is dead: a large frame
// still arriving keeps it alive (linkheard), however slow the link.
//
// On each new stream both sides first send REPLAY, a replay request:
// its id is the first frame the sender still keeps, its ack the last
// frame it received, and its payload its token, 8 random bytes naming
// its side of the link. Each side then drops what the other has and
// sends the rest again in order. Frames past what a receiver expects
// were dropped for good (LINK_LOST), and a token unlike the last means
// the peer is a new instance (LINK_FRESH), a host that started over or
// forgot this client: what was kept for the old one is dropped unsent,
// since the new one never saw what came before it, and numbering goes
// on from the replay request's id. Either way the layer above catches
// up from the logs. Until the peer's replay request comes, a side sends
// nothing but its own, and a goodbye: its acknowledgements would count
// in numbers the peer, if new, does not share. For the same reason a
// replay request acknowledging frames never sent counts another
// instance's, and drops nothing. DISCONNECT ends the link for good.
// Anything malformed breaks the stream (LINK_BROKEN), which its owner
// then closes, to be replaced.
//
// Nothing here does I/O or reads a clock: callers feed in the bytes
// received and the time, and write out what linkout gives them.
//
// This is free and unencumbered software released into the public domain.

enum : u8 {
    FRAME_REGULAR    = 1,
    FRAME_ACK        = 3,
    FRAME_DISCONNECT = 5,
    FRAME_REPLAY     = 6,
    FRAME_KEEPALIVE  = 9,
};

enum : iz {
    FRAME_HEAD   = 13,
    FRAME_MAX    = (iz)8 << 20,  // payload of a regular frame
    FRAME_CTLMAX = 1 << 10,      // payload of a disconnect
    FRAME_TOKEN  = 8,            // payload of a replay request
};

enum : i64 {
    LINK_ACKTIME   =  2000000,   // os_ticks: acknowledge within this
    LINK_ACKEVERY  =  50,        // frames: or after this many
    LINK_KEEPALIVE =  5000000,   // os_ticks of silence before a keepalive
    LINK_DEAD      = 20000000,   // os_ticks hearing nothing: the stream is dead
};

// Link events (linkframe)
enum : i32 {
    LINK_NONE,     // nothing for the layer above
    LINK_DATA,     // a regular frame's payload, the next in order
    LINK_READY,    // the peer's replay request came: the link is up
    LINK_LOST,     // up, but frames the peer sent are gone for good
    LINK_FRESH,    // up, but the peer is a new instance
    LINK_BYE,      // the peer ended the link for good
    LINK_BROKEN,   // garbage or a protocol violation: drop the stream
};

static u32 frameget32(u8 *p)
{
    return (u32)p[0]<<24 | (u32)p[1]<<16 | (u32)p[2]<<8 | (u32)p[3];
}

static void frameput32(u8 *p, u32 v)
{
    p[0] = (u8)(v >> 24);
    p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >>  8);
    p[3] = (u8)(v >>  0);
}

static void framehead(u8 *p, u8 type, u32 id, u32 ack, iz len)
{
    p[0] = type;
    frameput32(p+1, id);
    frameput32(p+5, ack);
    frameput32(p+9, (u32)len);
}


// Decoding

// An incremental decoder for one stream. The buffer holds a frame that
// arrives in pieces, so it has room for the largest one.
struct FrameDec {
    u8 *buf;
    iz  max;   // payload limit of a regular frame
    iz  len;   // bytes of the current frame so far
    b32 bad;   // the stream broke: nothing more decodes
};

struct FrameIn {
    Str payload;  // valid until the next framenext
    u32 id;
    u32 ack;
    u8  type;
    b32 ok;       // a whole frame; else more input is needed, or bad
};

static FrameDec newframedec(Arena *perm, iz max)
{
    FrameDec d = {};
    d.max = max;
    d.buf = allocbytes(perm, FRAME_HEAD+max);
    return d;
}

// A header's payload length if the header is valid, else -1.
static iz framelen(u8 *h, iz max)
{
    u32 len = frameget32(h+9);
    switch (h[0]) {
    case FRAME_REGULAR:    return len<=(u32)max ? (iz)len : -1;
    case FRAME_ACK:        return len==0 ? 0 : -1;
    case FRAME_KEEPALIVE:  return len==0 ? 0 : -1;
    case FRAME_REPLAY:     return len==FRAME_TOKEN ? (iz)FRAME_TOKEN : -1;
    case FRAME_DISCONNECT: return len<=FRAME_CTLMAX ? (iz)len : -1;
    }
    return -1;
}

// Decode the next frame from the input, consuming what it uses. A frame
// within the input is returned in place; one split across inputs is
// gathered in the buffer.
static FrameIn framenext(FrameDec *d, Str *in)
{
    FrameIn r = {};
    if (d->bad) {
        return r;
    }

    if (!d->len && in->len>=FRAME_HEAD) {
        iz len = framelen(in->data, d->max);
        if (len < 0) {
            d->bad = 1;
            return r;
        } else if (in->len-FRAME_HEAD >= len) {
            r.type    = in->data[0];
            r.id      = frameget32(in->data+1);
            r.ack     = frameget32(in->data+5);
            r.payload = {in->data+FRAME_HEAD, len};
            r.ok      = 1;
            *in = cuthead(*in, FRAME_HEAD+len);
            return r;
        }
    }

    for (;;) {
        iz need = FRAME_HEAD - d->len;
        if (need <= 0) {
            iz len = framelen(d->buf, d->max);
            if (len < 0) {
                d->bad = 1;
                return r;
            }
            need = FRAME_HEAD + len - d->len;
            if (!need) {
                r.type    = d->buf[0];
                r.id      = frameget32(d->buf+1);
                r.ack     = frameget32(d->buf+5);
                r.payload = {d->buf+FRAME_HEAD, len};
                r.ok      = 1;
                d->len    = 0;
                return r;
            }
        }
        if (!in->len) {
            return r;
        }
        iz take = need<in->len ? need : in->len;
        copybytes(d->buf+d->len, in->data, take);
        d->len += take;
        *in = cuthead(*in, take);
    }
}


// Links

// One side of a link, outliving the streams that carry it. Regular
// frames are kept encoded in a ring of keepmax bytes, at offsets that
// only grow, [qhead, qtail), oldest first, of which those in [qsent,
// qtail) are not yet written to the current stream.
struct Link {
    iz   keepmax;     // bytes of frames kept, and the size of q
    i32  keepcount;   // frames kept at most
    u64  token;       // this side's, nonzero
    u8  *q;           // the ring: offset i at q[i % keepmax]
    i64  qhead;
    i64  qsent;
    i64  qtail;
    i32  qcount;
    u32  firstid;     // the frame at qhead, or nextid if none
    u32  nextid;      // the next regular frame's id
    b32  ready;       // the peer's replay request came on this stream
    b32  wantreplay;  // control frames waiting for linkout
    b32  wantack;
    b32  wantkeep;
    b32  wantbye;
    u32  recvd;       // the last regular frame received in order
    i32  owed;        // frames received since an ack last went out
    i64  oweby;       // ticks: acknowledge by then, if owed
    u64  peer;        // the peer's token, 0 until its first request
    i64  sentat;      // ticks of the last bytes written
    i64  heardat;     // ticks of the last bytes received
};

static Link newlink(Arena *perm, u64 token, iz keepmax, i32 keepcount)
{
    Link l = {};
    l.keepmax   = keepmax;
    l.keepcount = keepcount;
    l.token     = token ? token : 1;
    l.q         = allocbytes(perm, keepmax);
    l.firstid   = 1;
    l.nextid    = 1;
    return l;
}

// Copy between the ring at an offset and flat memory, either way.
static void linkring(Link *l, i64 off, u8 *p, iz len, b32 put)
{
    iz at    = (iz)(off % l->keepmax);
    iz first = l->keepmax-at<len ? l->keepmax-at : len;
    u8 *q    = l->q + at;
    copybytes(put ? q : p, put ? p : q, first);
    copybytes(put ? l->q : p+first, put ? p+first : l->q, len-first);
}

// Bytes free for kept frames, header included.
static iz linkroom(Link *l)
{
    return l->qcount<l->keepcount ? l->keepmax - (iz)(l->qtail - l->qhead) : 0;
}

// Queue a line as a regular frame, to go out with the next linkout once
// the link is up. Returns 0, taking nothing, past the limits.
static b32 linksend(Link *l, Str line)
{
    iz need = FRAME_HEAD + line.len;
    if (line.len>FRAME_MAX || need>linkroom(l)) {
        return 0;
    }
    u8 h[FRAME_HEAD];
    framehead(h, FRAME_REGULAR, l->nextid++, l->recvd, line.len);
    linkring(l, l->qtail, h, FRAME_HEAD, 1);
    linkring(l, l->qtail+FRAME_HEAD, line.data, line.len, 1);
    l->qtail += need;
    l->qcount++;
    return 1;
}

// A new stream carries the link from now on: it begins with a replay
// request, and kept frames wait for the peer's.
static void linkconnect(Link *l, i64 now)
{
    l->ready      = 0;
    l->qsent      = l->qhead;
    l->wantreplay = 1;
    l->wantack    = 0;
    l->wantkeep   = 0;
    l->sentat     = now;
    l->heardat    = now;
}

// End the link for good, after whatever is queued.
static void linkbye(Link *l)
{
    l->wantbye = 1;
}

// Drop kept frames the peer acknowledged. Returns 0 if it acknowledged
// a frame never sent.
static b32 linktrim(Link *l, u32 ack)
{
    if ((i32)(ack - (l->nextid-1)) > 0) {
        return 0;
    }
    while (l->qcount && (i32)(ack - l->firstid)>=0) {
        u8 len[4];
        linkring(l, l->qhead+9, len, 4, 0);
        l->qhead += FRAME_HEAD + (iz)frameget32(len);
        l->qsent  = l->qsent>l->qhead ? l->qsent : l->qhead;
        l->qcount--;
        l->firstid++;
    }
    return 1;
}

// Drop every frame kept, unsent, for a peer gone for good: the next is
// numbered as the first of them was.
static void linkforget(Link *l)
{
    l->qhead  = l->qsent = l->qtail;
    l->qcount = 0;
    l->nextid = l->firstid;
}

// Bytes arrived on the current stream, a whole frame or not: it lives.
static void linkheard(Link *l, i64 now)
{
    l->heardat = now;
}

// Take one frame received on the current stream, which begins with the
// peer's replay request. Its acknowledgement counts only from a peer
// known before: a first or new instance numbers this side's frames
// from nothing. A known peer's may count those of this side's previous
// instance, a host that started over, which never reached it: past
// what this one sent, it drops nothing, and the peer goes LINK_FRESH on
// this side's request.
static i32 linkframe(Link *l, FrameIn *f, i64 now)
{
    l->heardat = now;
    if (f->type == FRAME_REPLAY) {
        if (l->ready) {
            return LINK_BROKEN;  // one per stream
        }
        u8 *p     = f->payload.data;
        u64 token = (u64)frameget32(p)<<32 | frameget32(p+4);
        i32 r     = LINK_READY;
        if (!l->peer || token!=l->peer) {
            r        = l->peer ? LINK_FRESH : LINK_READY;
            l->recvd = f->id - 1;  // from where it is
            if (r == LINK_FRESH) {
                linkforget(l);
            }
        } else {
            linktrim(l, f->ack);  // nothing if another instance's
            if ((i32)(f->id - (l->recvd+1)) > 0) {
                l->recvd = f->id - 1;
                r = LINK_LOST;
            }
        }
        l->peer  = token;
        l->ready = 1;
        l->qsent = l->qhead;  // everything kept goes again
        return r;
    }
    if (!l->ready) {
        return LINK_BROKEN;
    } else if (f->type == FRAME_DISCONNECT) {
        return LINK_BYE;  // whatever it acknowledges: that is moot now
    } else if (!linktrim(l, f->ack)) {
        return LINK_BROKEN;
    }
    switch (f->type) {
    case FRAME_REGULAR: {
        i32 d = (i32)(f->id - l->recvd);
        if (d > 1) {
            return LINK_BROKEN;  // out of order: start over on a new stream
        } else if (d <= 0) {
            return LINK_NONE;    // sent again, received already
        }
        l->recvd = f->id;
        if (!l->owed++) {
            l->oweby = now + LINK_ACKTIME;
        }
        l->wantack |= l->owed >= LINK_ACKEVERY;
        return LINK_DATA;
    }
    case FRAME_ACK:
    case FRAME_KEEPALIVE:
        return LINK_NONE;
    }
    return LINK_BROKEN;
}

// Queue the control frames now due: an acknowledgement owed, or a
// keepalive after silence, once the peer's replay request came. Returns
// 0 if the stream is dead, having brought nothing for LINK_DEAD.
static b32 linktick(Link *l, i64 now)
{
    if (now-l->heardat >= LINK_DEAD) {
        return 0;
    } else if (l->ready) {
        l->wantack  |= l->owed && now>=l->oweby;
        l->wantkeep |= now-l->sentat >= LINK_KEEPALIVE;
    }
    return 1;
}

// When linktick next has something to do.
static i64 linkdeadline(Link *l)
{
    i64 r = l->heardat + LINK_DEAD;
    if (!l->ready) {
        return r;  // nothing to send before the peer's request
    }
    i64 k = l->sentat + LINK_KEEPALIVE;
    r = k<r ? k : r;
    if (l->owed && l->oweby<r) {
        r = l->oweby;
    }
    return r;
}

// Whether linkout has anything to write.
static b32 linkpending(Link *l)
{
    return l->wantreplay || l->wantbye ||
           (l->ready && (l->wantack || l->wantkeep || l->qsent<l->qtail));
}

// The bytes to write to the current stream now, in order, each frame
// acknowledging all received so far. Empty if nothing is due. Before
// the peer's replay request, only this side's and a goodbye.
static Str linkout(Link *l, Arena *a, i64 now)
{
    if (!linkpending(l)) {
        return {};
    }
    iz  kept = l->ready ? (iz)(l->qtail - l->qsent) : 0;
    Buf b(a, 3*FRAME_HEAD + FRAME_TOKEN + kept);
    if (l->wantreplay) {
        u8 p[FRAME_HEAD+FRAME_TOKEN];
        framehead(p, FRAME_REPLAY, l->firstid, l->recvd, FRAME_TOKEN);
        frameput32(p+FRAME_HEAD,   (u32)(l->token >> 32));
        frameput32(p+FRAME_HEAD+4, (u32)l->token);
        print(&b, {p, countof(p)});
    }
    if (kept) {
        u8 *dst = b.data + b.len;
        linkring(l, l->qsent, dst, kept, 0);
        b.len += kept;
        for (iz i = 0; i < kept;) {
            frameput32(dst+i+5, l->recvd);
            i += FRAME_HEAD + (iz)frameget32(dst+i+9);
        }
        l->qsent = l->qtail;
    }
    if (l->ready && l->wantack && !kept && !l->wantreplay) {
        u8 p[FRAME_HEAD];
        framehead(p, FRAME_ACK, 0, l->recvd, 0);
        print(&b, {p, countof(p)});
    } else if (l->ready && l->wantkeep && !b.len) {
        u8 p[FRAME_HEAD];
        framehead(p, FRAME_KEEPALIVE, 0, l->recvd, 0);
        print(&b, {p, countof(p)});
    }
    if (l->wantbye) {
        u8 p[FRAME_HEAD];
        framehead(p, FRAME_DISCONNECT, 0, l->recvd, 0);
        print(&b, {p, countof(p)});
    }
    l->wantreplay = l->wantack = l->wantkeep = l->wantbye = 0;
    l->owed   = 0;
    l->sentat = now;
    return finish(&b);
}
