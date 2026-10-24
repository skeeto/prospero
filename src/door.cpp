// One window per state directory
//
// A window takes the lock run/gui.lock of its state directory (guilock)
// for the life of its process, before anything else of that directory,
// and opens its door: a listener at os_endpoint(STATE, "gui-p0"), on a
// thread of its own (opendoor). A launch that finds the lock held knocks
// at the door (knock), and goes:
//
//   newcomer: {"raise":1,"token":T}   T: its XDG_ACTIVATION_TOKEN, if any
//   window:   {"ok":1,"pid":N}
//   newcomer: {"go":1}                after AllowSetForegroundWindow(N)
//   window:   ends the connection,    having posted the raise
//
// The window takes the raise as a record of the type it chose in its
// mailbox, with the token as payload, and comes forward as the tray's
// Open brings it. A connection that ends after the raise request without
// "go" raises all the same. A newcomer that finds nobody at the door
// (the window still starting) tries again for a while. The kernel lets
// the lock go with its process, however that ends, and a door left by a
// dead process refuses connections until the next window replaces it.
//
// This is free and unencumbered software released into the public domain.

enum : i32 {
    DOOR_LINE  = 1<<12,  // longest request line
    DOOR_TOKEN = 256,    // longest activation token passed on
    DOOR_WAIT  = 5000,   // ms for the window to answer each step
};

static Str guilockpath(Arena *a, Str statedir)
{
    return join(a, join(a, statedir, "run"), "gui.lock");
}

// Take the window's lock on a state directory, making its run directory
// as needed: the handle, or -1 if another process holds it, or the
// directory cannot be made (os_lockheld tells which).
static iz guilock(Os *os, Arena scratch, Str statedir)
{
    Str run  = join(&scratch, statedir, "run");
    Str path = guilockpath(&scratch, statedir);
    if (!os_mkdirs(os, scratch, run)) {
        return -1;
    }
    return os_lockfile(os, scratch, path);
}

static Str doorendpoint(Os *os, Arena *a, Str statedir)
{
    return os_endpoint(os, a, statedir, "gui-p0");
}

enum : i32 {
    DOOR_BAD,
    DOOR_RAISE,
    DOOR_GO,
};

struct DoorReq {
    Str token;  // DOOR_RAISE: printable ASCII, else empty
    i32 kind;
};

// A newcomer's request line. A token that is too long or not printable
// ASCII is dropped, as it goes into the environment.
static DoorReq doorparse(Arena *a, Str line)
{
    DoorReq r   = {};
    JsonDoc doc = jsonparse(a, line);
    if (!doc.ok || doc.root->type!=JSON_OBJECT) {
        return r;
    }
    if (jsonint(jsonget(doc.root, "go"), 0) == 1) {
        r.kind = DOOR_GO;
        return r;
    } else if (jsonint(jsonget(doc.root, "raise"), 0) != 1) {
        return r;
    }
    Str tok = jsonstr(jsonget(doc.root, "token"));
    b32 ok  = tok.len <= DOOR_TOKEN;
    for (iz i = 0; ok && i < tok.len; i++) {
        ok = tok[i]>0x20 && tok[i]<0x7f;
    }
    r.token = ok ? tok : Str{};
    r.kind  = DOOR_RAISE;
    return r;
}

struct DoorConn {
    DoorConn *next;
    Conn     *c;       // null once hung up, until its CONN_EOF
    u64       id;
    u8       *line;    // DOOR_LINE bytes
    iz        len;
    u8       *token;   // DOOR_TOKEN bytes
    iz        toklen;
    b32       asked;   // a raise request came
    b32       raised;  // and was posted
};

struct Door {
    Os       *os;
    Mailbox  *mb;      // the listener's records
    Mailbox  *done;    // takes the raises
    u32       type;
    i64       pid;
    Arena     perm;    // connections, reused through free
    Arena     scratch;
    DoorConn *conns;
    DoorConn *free;
};

static void doorraise(Door *d, DoorConn *k)
{
    if (k->asked && !k->raised) {
        k->raised = 1;
        mailpost(d->done, d->type, 0, {k->token, k->toklen}, 1);
    }
}

static void doorhangup(Door *d, DoorConn *k)
{
    if (k->c) {
        os_connclose(d->os, k->c);
        k->c = 0;
    }
}

// One complete line from a newcomer.
static void doorline(Door *d, DoorConn *k, Str line)
{
    Arena   a   = d->scratch;
    DoorReq req = doorparse(&a, line);
    switch (req.kind) {
    case DOOR_RAISE: {
        if (k->asked) {
            doorhangup(d, k);
            return;
        }
        k->asked  = 1;
        k->toklen = req.token.len;
        copybytes(k->token, req.token.data, req.token.len);
        Buf b(&a, 64);
        print(&b, "{\"ok\":1,\"pid\":");
        print(&b, d->pid);
        print(&b, "}\n");
        os_connwrite(d->os, k->c, finish(&b));
    } return;
    case DOOR_GO:
        if (!k->asked) {
            doorhangup(d, k);
            return;
        }
        doorraise(d, k);
        os_connwrite(d->os, k->c, {});  // the end tells it was heard
        return;
    }
    doorhangup(d, k);
}

static void doorthread(void *arg)
{
    Door *d = (Door *)arg;
    for (;;) {
        mailwait(d->mb, -1);
        Mail m = mailtake(d->mb);
        for (Record r; (r = mailnext(&m)).ok;) {
            DoorConn **pk = &d->conns;
            for (; *pk && (*pk)->id!=r.id; pk = &(*pk)->next) {}
            DoorConn *k = *pk;
            switch (r.type) {
            case CONN_NEW: {
                if (d->free) {
                    k = d->free;
                    d->free = k->next;
                } else {
                    k = alloc<DoorConn>(&d->perm);
                    k->line  = allocbytes(&d->perm, DOOR_LINE);
                    k->token = allocbytes(&d->perm, DOOR_TOKEN);
                }
                Conn *c = 0;
                copybytes(&c, r.data.data, sizeof(c));
                u8 *line  = k->line;
                u8 *token = k->token;
                *k = {};
                k->line  = line;
                k->token = token;
                k->c     = c;
                k->id    = r.id;
                k->next  = d->conns;
                d->conns = k;
            } break;
            case CONN_DATA:
                for (iz i = 0; k && k->c && i < r.data.len; i++) {
                    u8 c = r.data[i];
                    if (c == '\n') {
                        doorline(d, k, {k->line, k->len});
                        k->len = 0;
                    } else if (k->len == DOOR_LINE) {
                        doorhangup(d, k);
                    } else {
                        k->line[k->len++] = c;
                    }
                }
                break;
            case CONN_EOF:
                if (k) {
                    doorraise(d, k);  // asked, but gone before "go"
                    doorhangup(d, k);
                    *pk     = k->next;
                    k->next = d->free;
                    d->free = k;
                }
                break;
            }
        }
    }
}

// Open the door of the window that holds statedir (guilock), for good:
// raises go to done as records of the given type. Returns why not, or
// an empty string.
static Str opendoor(Os *os, Arena *perm, Arena scratch, Str statedir,
                    Mailbox *done, u32 type)
{
    Region *rg = os_reserve(os, (iz)1<<24);
    if (!rg) {
        return "out of memory";
    }
    Door *d = alloc<Door>(perm);
    d->os      = os;
    d->mb      = newmailbox(perm, 1<<16, 0, 0);
    d->done    = done;
    d->type    = type;
    d->pid     = os_pid(os);
    d->perm    = newarena(rg);
    d->scratch = newarena(os_reserve(os, (iz)1<<20));
    Str ep = doorendpoint(os, perm, statedir);
    if (!d->mb || !d->scratch.beg) {
        return "out of memory";
    } else if (!ep.len) {
        return concat(perm, "no place for its endpoint: ", os_lasterr(os, &scratch));
    } else if (!os_listen(os, scratch, ep, d->mb)) {
        return concat(perm, "cannot listen: ", os_lasterr(os, &scratch));
    } else if (!os_thread(os, doorthread, d)) {
        return "cannot start its thread";
    }
    return {};
}

struct Knock {
    i64 pid;  // the window's process
    b32 ok;   // it answered, and comes forward
};

// Wait for records of connection 1 until the condition holds or the
// deadline passes. Returns 1 if it held.
static b32 knockwait(Os *os, Mailbox *mb, Buf *got, b32 *eof, i64 deadline, b32 line)
{
    for (;;) {
        iz nl = 0;
        for (; nl<got->len && got->data[nl]!='\n'; nl++) {}
        if (*eof || (line && nl<got->len)) {
            return line ? nl<got->len : 1;
        }
        i64 left = deadline - os_ticks(os);
        if (left <= 0) {
            return 0;
        }
        mailwait(mb, (i32)(left/MS + 1));
        Mail m = mailtake(mb);
        for (Record r; (r = mailnext(&m)).ok;) {
            if (r.type==CONN_DATA && got->len+r.data.len<=DOOR_LINE) {
                print(got, r.data);
            } else if (r.type == CONN_EOF) {
                *eof = 1;
            }
        }
    }
}

// Knock at the door of the window that holds statedir, trying for up to
// ms to find it, and ask it to come forward, passing on token (Wayland's
// activation token, or empty). With letfront, the window's process may
// take the foreground before it is told to go on (Windows). The mailbox
// goes in perm, as the connection's records may outlive the call.
static Knock knock(Os *os, Arena *perm, Arena scratch, Str statedir, Str token,
                   i32 ms, void (*letfront)(Os *, i64 pid))
{
    Knock    r  = {};
    Str      ep = doorendpoint(os, &scratch, statedir);
    Mailbox *mb = newmailbox(perm, 1<<14, 0, 0);
    if (!ep.len || !mb) {
        return r;
    }
    i64   deadline = os_ticks(os) + (i64)ms*MS;
    Conn *c        = 0;
    while (!(c = os_connect(os, scratch, ep, mb, 1))) {
        if (os_ticks(os) >= deadline) {
            return r;
        }
        mailwait(mb, 50);
    }

    Buf     req(&scratch, 64 + token.len);
    JsonOut w = {&req};
    putobject(&w);
    putintkey(&w, "raise", 1);
    putstrkey(&w, "token", token);
    putend(&w);
    putbyte(&req, '\n');
    Buf got(&scratch, DOOR_LINE);
    b32 eof = 0;
    os_connwrite(os, c, finish(&req));
    if (!knockwait(os, mb, &got, &eof, os_ticks(os) + (i64)DOOR_WAIT*MS, 1)) {
        os_connclose(os, c);
        return r;
    }
    iz nl = 0;
    for (; got.data[nl] != '\n'; nl++) {}
    Arena   a     = scratch;
    JsonDoc reply = jsonparse(&a, {got.data, nl});
    i64     pid   = jsonint(jsonget(reply.root, "pid"), 0);
    if (jsonint(jsonget(reply.root, "ok"), 0)!=1 || pid<=0) {
        os_connclose(os, c);
        return r;
    }
    if (letfront) {
        letfront(os, pid);
    }
    os_connwrite(os, c, "{\"go\":1}\n");
    r.pid = pid;
    r.ok  = knockwait(os, mb, &got, &eof, os_ticks(os) + (i64)DOOR_WAIT*MS, 0);
    os_connclose(os, c);
    return r;
}
