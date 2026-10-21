// Mailboxes: thread-to-thread record queues
//
// Producers copy records into the front buffer under a lock. The consumer
// swaps the two buffers under the lock and reads the full one outside of
// it, so neither side holds the lock while doing real work. A record is
// a 16-byte header, {u32 type; u32 len; u64 id}, then len payload bytes
// padded to a multiple of 8.
//
// The consumer is woken through the ready event, or through a notify
// hook in its place (the GUI wakes with os_wake). Only the first record
// or drop after a take wakes it, so it must take everything after each
// wake. Another consumer may take over, by the other means (mailnotify):
// the GUI's inbox goes to a host thread when its engine is kept running
// in the background.
//
// A producer either waits for room (backpressure: HTTP body chunks) or
// never blocks and drops the record when the buffer is full (engine
// events for the GUI). A drop sets the overflow flag, and later
// non-blocking records are dropped too until the next take. So the
// non-blocking records lost before a take were all posted after the
// non-blocking records it returns, while blocking records are still
// accepted and may follow the gap. Only the consumer makes room, so it
// must never block posting to itself.
//
// This is free and unencumbered software released into the public domain.

enum : iz {
    MAIL_HEADER = 16,
};

struct MailHeader {
    u32 type;
    u32 len;
    u64 id;
};
static_assert(sizeof(MailHeader) == MAIL_HEADER);

struct Mailbox {
    Lock    lock;
    Event   ready;     // the first record or drop since the last take
    Event   space;     // a take made room, or a woken producer passes it on
    void  (*notify)(void *);  // replaces the ready event when set
    void   *ctx;
    u8     *buf[2];
    iz      cap;       // bytes per buffer
    iz      len;       // bytes used in the front buffer
    i32     front;     // the buffer producers fill
    i32     waiting;   // producers blocked for room
    b32     overflow;  // records dropped since the last take
};

// Records from one take, read in order with mailnext.
struct Mail {
    u8 *data;
    iz  len;
    iz  off;       // read position
    b32 overflow;  // records were dropped since the previous take
};

struct Record {
    Str data;
    u64 id;
    u32 type;
    b32 ok;  // 0 past the last record
};

// A mailbox with two buffers of cap bytes, which also bounds the size of
// one record. With notify set, producers call notify(ctx) instead of
// signaling the ready event. Returns null if events cannot be created.
static Mailbox *newmailbox(Arena *perm, iz cap,
                           void (*notify)(void *), void *ctx)
{
    assert(cap>=MAIL_HEADER && cap<=(iz)0x7ffffff8);
    cap += -cap & 7;
    Mailbox *m = alloc<Mailbox>(perm);
    if (!os_eventinit(&m->ready) || !os_eventinit(&m->space)) {
        return 0;
    }
    os_lockinit(&m->lock);
    m->notify = notify;
    m->ctx    = ctx;
    m->cap    = cap;
    m->buf[0] = (u8 *)alloc<u64>(perm, cap/8);
    m->buf[1] = (u8 *)alloc<u64>(perm, cap/8);
    return m;
}

// Hand the consumer's side to another kind of consumer: a notify hook in
// place of the ready event, or none for mailwait. The ready event is
// signaled after, as a record posted before the change may have notified
// the old consumer instead, so that the new one takes a look at once.
static void mailnotify(Mailbox *m, void (*notify)(void *), void *ctx)
{
    os_lock(&m->lock);
    m->notify = notify;
    m->ctx    = ctx;
    os_unlock(&m->lock);
    os_signal(&m->ready);
}

// The largest payload a record may carry.
static iz mailmax(Mailbox *m)
{
    return m->cap - MAIL_HEADER;
}

// How to wake the consumer, read under the lock, since mailnotify may
// change it, and used once the lock is let go.
struct MailWake {
    void (*notify)(void *);
    void  *ctx;
};

static void mailwake(Mailbox *m, MailWake w)
{
    if (w.notify) {
        w.notify(w.ctx);
    } else {
        os_signal(&m->ready);
    }
}

// Append a record. With block set, wait while there is no room for it;
// otherwise drop it and set the overflow flag. Returns 0 if the record
// was dropped, including one longer than mailmax, which is always
// dropped and also sets the flag.
static b32 mailpost(Mailbox *m, u32 type, u64 id, Str data, b32 block)
{
    os_lock(&m->lock);
    if (data.len > mailmax(m)) {
        // Into an empty mailbox, no record will wake the consumer, which
        // must still learn of the drop.
        b32      wake = !m->len && !m->overflow;
        MailWake how  = {m->notify, m->ctx};
        m->overflow = 1;
        os_unlock(&m->lock);
        if (wake) {
            mailwake(m, how);
        }
        return 0;
    }

    iz  need   = MAIL_HEADER + data.len + (-data.len & 7);
    b32 waited = 0;
    for (;;) {
        if (!block && m->overflow) {
            os_unlock(&m->lock);
            return 0;
        } else if (need <= m->cap - m->len) {
            break;
        } else if (!block) {
            // Any record fits an empty buffer, so this buffer holds a
            // record whose post already woke the consumer.
            m->overflow = 1;
            os_unlock(&m->lock);
            return 0;
        }
        m->waiting++;
        os_unlock(&m->lock);
        os_wait(&m->space, -1);
        os_lock(&m->lock);
        m->waiting--;
        waited = 1;
    }

    u8        *dst = m->buf[m->front] + m->len;
    MailHeader hdr = {type, (u32)data.len, id};
    copybytes(dst, &hdr, MAIL_HEADER);
    copybytes(dst+MAIL_HEADER, data.data, data.len);
    for (iz i = MAIL_HEADER+data.len; i < need; i++) {
        dst[i] = 0;
    }
    b32      wake = !m->len && !m->overflow;
    b32      pass = waited && m->waiting;  // the take woke only one of them
    MailWake how  = {m->notify, m->ctx};
    m->len += need;
    os_unlock(&m->lock);

    if (wake) {
        mailwake(m, how);
    }
    if (pass) {
        os_signal(&m->space);
    }
    return 1;
}

// Take every record posted since the last take. They remain valid until
// the next take, which hands their buffer back to producers.
static Mail mailtake(Mailbox *m)
{
    Mail r = {};
    os_lock(&m->lock);
    r.data      = m->buf[m->front];
    r.len       = m->len;
    r.overflow  = m->overflow;
    m->front   ^= 1;
    m->len      = 0;
    m->overflow = 0;
    b32 wake = m->waiting > 0;
    os_unlock(&m->lock);

    if (wake) {
        os_signal(&m->space);
    }
    return r;
}

// The next record of a take, or one with ok clear after the last.
static Record mailnext(Mail *m)
{
    Record r = {};
    if (m->off == m->len) {
        return r;
    }
    assert(m->len-m->off >= MAIL_HEADER);
    MailHeader hdr = {};
    copybytes(&hdr, m->data+m->off, MAIL_HEADER);
    r.data = {m->data + m->off + MAIL_HEADER, (iz)hdr.len};
    r.id   = hdr.id;
    r.type = hdr.type;
    r.ok   = 1;
    m->off += MAIL_HEADER + r.data.len + (-r.data.len & 7);
    assert(m->off <= m->len);
    return r;
}

// Wait up to ms milliseconds (forever if negative) for records or a drop
// without taking them. Returns 1 if woken. Not for mailboxes with a
// notify hook.
static b32 mailwait(Mailbox *m, i32 ms)
{
    return os_wait(&m->ready, ms);
}
