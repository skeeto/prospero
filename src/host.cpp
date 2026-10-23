// Engine host: "prospero serve", "prospero attach" and "prospero hosts"
//
//   prospero serve [options]               the engine, behind a local endpoint
//   prospero attach [--reconnect] [opts]   standard I/O to the endpoint
//   prospero hosts [--state DIR]           the hosts running, as JSONL
//
// A host is the engine with a local listener (os_listen): one per state
// directory and protocol version, at os_endpoint(STATE, "host-pN"),
// guarded by the lock file run/host-pN.lock, and registered while it runs
// as hosts/PID.ini (pid, endpoint, version, proto, started). A client
// reaches it through a local connection, or over ssh through "prospero
// attach", which bridges its standard I/O to the endpoint, starting a
// host when none runs. Attach starts the host detached (os_detach), so
// that it outlives the ssh session, its errors in run/host.log, and
// waits up to 10 s for it to listen: a host takes its lock before all
// else, and never waits for URL includes not yet cached (config.cpp),
// which its engine fetches meanwhile. With --reconnect it starts nothing:
// exit status 90 says that no host runs, so the client knows that what
// it had there is gone. Anyone who finds a registered host unreachable
// removes its registration.
//
// Each client speaks frames (frame.cpp) carrying protocol v0, as one
// link that outlives its streams: a client whose stream breaks keeps
// its place, its records queued, for [host] grace (3 h), and takes it up
// again from where it was on its next stream, which names it by its
// token. A disconnect frame, or the grace running out, ends it, as does
// a reply finding its queue full; a client that comes back after that
// finds a new link (LINK_FRESH), as it would a new host. The host's
// hello names its protocol (proto, the major version), and refuses a
// client that names another.
//
// Several clients may attach at once, and each receives what it asks
// for. Commands go to the engine with ids of the host's own, and their
// replies come back, in order, to whoever asked under the asker's ids;
// records without a session (the sessions list) go to the asker of the
// command they answer. Session records go to the clients that opened
// the session (open or new), or one it is a subagent of, under the
// session's own sequence numbers: a client's records for a session come
// live from the engine while they follow on from what it has, and from
// the session's log otherwise, as after an open from a seq (the engine
// replays nothing itself), records lost to a full inbox, or a link too
// backed up to take more (said in the log). A client catching up from
// the log is fed as its link drains, and once caught up the engine is
// asked for the state of a session at work, with a snapshot of what
// streams, if anything. A session is attended
// while any client has it open attended, and it closes, stopping its
// jobs, when the last client that opened it closes it; a client that
// goes away for good closes nothing, and a turn without clients goes on.
//
// The host exits after [host] idle_exit (10 min) with no client
// connected and nothing running (the engine's status: no turn, no job),
// and when a client quits it ({"cmd":"quit"}), which stops the engine as
// quit always does, as do SIGTERM, SIGINT and SIGHUP (on Windows, its
// console's Ctrl+C, Ctrl+Break or closing): either way it unlistens and
// removes its registration. Its errors and notes go to run/host.log,
// which a host starting when it is past 1 MiB starts over.
//
// A GUI whose window closes with "Keep running in background" becomes a
// host in place (newhost, then hostrun on a thread of its own): its
// engine runs on as it was, its records, which the window took, going
// to the host from then on, and the host's ids starting above the
// window's. Another thread may ask the host to stop every turn and job
// or to quit (hoststop), as a tray icon's menu does.
//
// This is free and unencumbered software released into the public domain.

enum : i64 {
    HOST_NOSERVER  = 90,           // attach --reconnect: no host runs
    HOST_PROTO     = 0,            // protocol version, major
    HOST_WIDE      = sizeof(void *) == 8,  // else 32-bit: XP's 2 GiB
    HOST_KEEPMAX   = (i64)(HOST_WIDE ? 32 : 16) << 20,  // bytes kept per client
    HOST_KEEPCOUNT = 1 << 16,
    HOST_CHUNK     = (i64)1 << 20, // log bytes read at a time to catch up
    HOST_HELLO     = 10*SEC,       // for a new stream's replay request
    HOST_CHECK     = 5*SEC,        // between status checks while quiet
    HOST_READY     = 10*SEC,       // attach: for a host it started
    HOST_INBOX     = (i64)(HOST_WIDE ? 8 : 4) << 20,
    HOST_SCRATCH   = HOST_WIDE ? (i64)1<<32 : (i64)1<<26,
    HOST_CLIENTMEM = HOST_KEEPMAX + ((i64)(HOST_WIDE ? 64 : 8) << 20),
    HOST_LOGMAX    = (i64)1 << 20, // run/host.log, before a host starts it over
};

// A frame of any size must fit in a client's empty queue, with room to
// spare for replies (hostreserve), so that catching up always makes
// progress.
static_assert(HOST_KEEPMAX >= FRAME_HEAD + FRAME_MAX + HOST_KEEPMAX/32);

enum : i32 {
    HOST_CMDS = 4096,  // commands awaiting replies, at most
};

// Records other threads post to a host's inbox (hoststop)
enum : u32 {
    HOST_STOPALL = 400,  // stop every turn and job, and run on
    HOST_QUITALL,        // stop the engine, and so the host
};

static Str hostname(Arena *a, i32 proto)
{
    Buf b(a, 16);
    print(&b, "host-p");
    print(&b, (i64)proto);
    return finish(&b);
}

static void hostsay(Os *os, Arena scratch, Str msg, Str detail = {})
{
    Buf b(&scratch, 128 + msg.len + detail.len);
    print(&b, "prospero: ");
    print(&b, msg);
    if (detail.len) {
        print(&b, ": ");
        print(&b, detail);
    }
    putbyte(&b, '\n');
    os_print(os, 2, finish(&b));
}


// Registration: hosts/PID.ini

struct HostEntry {
    HostEntry *next;
    Str        path;      // the registration file
    Str        endpoint;
    Str        version;
    i64        pid;
    i64        proto;
    i64        started;   // Unix epoch milliseconds
};

static Str hostsdir(Arena *a, Str statedir)
{
    return join(a, statedir, "hosts");
}

static Str hostentry(Arena *a, i64 pid, Str endpoint, i64 started, i32 proto)
{
    Buf b(a, 256 + endpoint.len);
    print(&b, "pid = ");
    print(&b, pid);
    print(&b, "\nendpoint = ");
    print(&b, endpoint);
    print(&b, "\nversion = " PROSPERO_VERSION "\nproto = ");
    print(&b, (i64)proto);
    print(&b, "\nstarted = ");
    print(&b, started);
    putbyte(&b, '\n');
    return finish(&b);
}

// The registered hosts, live or not, oldest first.
static HostEntry *hostentries(Os *os, Arena *a, Str statedir)
{
    HostEntry  *head = 0;
    HostEntry **tail = &head;
    Str         dir  = hostsdir(a, statedir);
    for (Dirent *d = os_list(os, a, dir); d; d = d->next) {
        if (d->isdir || !endswith(d->name, ".ini")) continue;
        Str  path = join(a, dir, d->name);
        Read rd   = os_read(os, a, path, 1<<12);
        if (rd.err != READ_OK) continue;
        HostEntry *e = alloc<HostEntry>(a);
        e->path = path;
        for (Str rest = rd.data; rest.len;) {
            Cut line = cut(rest, '\n');
            Cut kv   = cut(line.head, '=');
            rest = line.tail;
            Str k = trim(kv.head);
            Str v = trim(kv.tail);
            if (k == "endpoint") {
                e->endpoint = v;
            } else if (k == "version") {
                e->version = v;
            } else if (k == "pid") {
                e->pid = parseint(v).val;
            } else if (k == "proto") {
                e->proto = parseint(v).val;
            } else if (k == "started") {
                e->started = parseint(v).val;
            }
        }
        *tail = e;
        tail  = &e->next;
    }
    return head;
}

// prospero hosts: the live hosts as JSONL, removing the rest.
static i32 hostsmain(Os *os, Arena perm, Arena scratch, Str *args, i32 nargs)
{
    Str state = {};
    for (i32 i = 2; i < nargs; i++) {
        if (args[i]=="--state" && i+1<nargs) {
            state = args[++i];
        } else {
            os_print(os, 2, "usage: prospero hosts [--state DIR]\n");
            return 2;
        }
    }
    state = state.len ? state : defaultstate(os, &perm);
    for (HostEntry *e = hostentries(os, &perm, state); e; e = e->next) {
        Arena a = scratch;
        if (!e->endpoint.len || !os_listening(os, a, e->endpoint)) {
            os_remove(os, a, e->path);  // stale
            continue;
        }
        Buf     b(&a, 512);
        JsonOut w = {};
        w.b = &b;
        putobject(&w);
        putintkey(&w, "pid", e->pid);
        putstrkey(&w, "endpoint", e->endpoint);
        putstrkey(&w, "version", e->version);
        putintkey(&w, "proto", e->proto);
        putintkey(&w, "started", e->started);
        putend(&w);
        putbyte(&b, '\n');
        os_print(os, 1, finish(&b));
    }
    return 0;
}


// The host's state

// A client's subscription to one session's records.
struct HostSub {
    HostSub *next;
    Str      sid;
    i64      sent;      // the last durable record given to the client
    i64      off;       // log offset to read from, with seq <= sent before it
    i64      newest;    // the newest live seq let by while catching up
    b32      catchup;   // its records come from the log until caught up
    b32      skip;      // off is inside a line too long to send
    b32      attended;
};

struct HostConn;

struct HostClient {
    HostClient *next;
    Region     *region;
    Arena       perm;
    u64         token;     // the client's, naming its link
    Link        link;
    HostConn   *conn;      // its current stream, if any
    i64         lostat;    // ticks: when it lost its stream
    HostSub    *subs;
    HostSub    *spare;     // subscriptions to reuse
    b32         gone;      // dropped: released at the end of the round
};

struct HostConn {
    HostConn   *next;
    Region     *region;
    Conn       *c;
    u64         id;
    FrameDec    dec;
    HostClient *client;    // named by its first frame, a replay request
    i64         since;     // ticks: when it connected
};

// What the host knows of a session from records passing through.
struct HostSess {
    Str parent;  // a subagent's parent session
    b32 busy;    // its live state is other than idle
};

enum : i32 {
    HOSTCMD_CLIENT,    // a client's, whose reply goes back to it
    HOSTCMD_STATUS,    // the host's own: for the idle exit
    HOSTCMD_QUIET,     // the host's own, with nothing to do on its reply
};

// A command sent to the engine, waiting for its reply.
struct HostCmd {
    i64         id;      // the host's, as the engine sees it
    i32         kind;
    i32         cmd;
    HostClient *client;
    i64         cid;     // the client's own id
    i64         from;    // open
    b32         attended;
    u8          sid[64];
    iz          sidlen;
};

struct Host {
    Os             *os;
    Arena           perm;
    Region         *scratch;
    Mailbox        *inbox;     // engine records, and connections'
    Engine         *e;
    Config         *cfg;
    Str             statedir;
    Str             endpoint;
    Str             regpath;
    Listener       *listener;
    iz              held;      // the lock that makes it the only host here
    HostConn       *conns;
    HostClient     *clients;
    HostClient     *dead;      // dropped this round, to release
    HostConn       *deadconns; // ended this round, to release
    Map<HostSess>  *sess;
    HostCmd        *cmds;      // a ring of HOST_CMDS
    i32             cmdhead;
    i32             cmdlen;
    i64             nextid;
    i64             idle;      // ticks of quiet before exiting
    i64             grace;     // ticks a client keeps its place without a stream
    i64             quietat;   // ticks: since then no client is connected, 0 if one is
    i64             busyat;    // ticks: when the engine last had work
    i64             checkat;   // ticks: ask the engine's status then, 0 if not
    b32             checking;  // a status command is out
    b32             overflow;  // records were lost: replies may be missing
    b32             quitting;  // the engine was told to stop
    i32             proto;     // the version in hello (a test hook changes it)
    iz              keepmax;   // bytes kept per client (a test hook changes it)
    u64             nextconn;
};

static HostSess *hostsess(Host *h, Str sid)
{
    HostSess *s = upsert(&h->sess, sid, (Arena *)0);
    if (!s) {
        s = upsert(&h->sess, clone(&h->perm, sid), &h->perm);
    }
    return s;
}

static HostSub *findsub(HostClient *c, Str sid)
{
    for (HostSub *s = c->subs; s; s = s->next) {
        if (s->sid == sid) {
            return s;
        }
    }
    return 0;
}

// Subscribe a client to a session, from a seq on, catching up from the
// log. A subscription already there starts over from there.
static HostSub *hostsub(HostClient *c, Str sid, i64 from, b32 attended)
{
    HostSub *s = findsub(c, sid);
    if (!s) {
        s = c->spare;
        if (s) {
            c->spare = s->next;
            Str keep = s->sid;
            *s = {};
            s->sid = keep;
        } else {
            s = alloc<HostSub>(&c->perm);
        }
        if (s->sid.len < sid.len) {
            s->sid = clone(&c->perm, sid);
        } else {
            copybytes(s->sid.data, sid.data, sid.len);
            s->sid.len = sid.len;
        }
        s->next = c->subs;
        c->subs = s;
    }
    s->sent     = from>0 ? from : 0;
    s->off      = 0;
    s->newest   = 0;
    s->catchup  = 1;
    s->skip     = 0;
    s->attended = attended;
    return s;
}

static void hostunsub(Host *, HostClient *, Arena scratch, Str sid);

// Whether any client attends the session.
static b32 hostattended(Host *h, Str sid)
{
    for (HostClient *c = h->clients; c; c = c->next) {
        HostSub *s = findsub(c, sid);
        if (s && s->attended) {
            return 1;
        }
    }
    return 0;
}

static b32 hostopened(Host *h, Str sid)
{
    for (HostClient *c = h->clients; c; c = c->next) {
        if (findsub(c, sid)) {
            return 1;
        }
    }
    return 0;
}


// Commands to the engine

static HostCmd *hostcmdat(Host *h, i32 i)
{
    return &h->cmds[(h->cmdhead + i) % HOST_CMDS];
}

// Send a command to the engine under a fresh id, the line as a parsed
// object with id replaced, and with from and attended replaced where
// given (from < 0 drops it). Null if too many wait already.
static HostCmd *hostforward(Host *h, Arena scratch, Json *j, i32 cmd, i32 kind,
                            i64 from, i32 attended)
{
    if (h->cmdlen == HOST_CMDS) {
        return 0;
    }
    HostCmd *q = hostcmdat(h, h->cmdlen++);
    *q = {};
    q->id   = ++h->nextid;
    q->kind = kind;
    q->cmd  = cmd;

    // An unknown command goes as it came, for the engine to refuse
    Buf     b(&scratch, 1024);
    JsonOut w = {};
    w.b = &b;
    putobject(&w);
    putkey(&w, "cmd");
    if (cmd == CMD_NONE) {
        putjson(&w, jsonget(j, "cmd"));
    } else {
        putstr(&w, cmdname(cmd));
    }
    putintkey(&w, "id", q->id);
    for (Json *m = j ? j->child : 0; m; m = m->next) {
        if (m->key=="cmd" || m->key=="id" ||
                (from>=0 && m->key=="from") ||
                (attended>=0 && m->key=="attended")) {
            continue;
        }
        putkey(&w, m->key);
        putjson(&w, m);
    }
    if (from >= 0) {
        putintkey(&w, "from", from);
    }
    if (attended >= 0) {
        putintkey(&w, "attended", attended);
    }
    putend(&w);
    enginecmd(h->e, finish(&b));
    return q;
}

// A command of the host's own, without a payload.
static HostCmd *hostown(Host *h, Arena scratch, i32 cmd, i32 kind, Str sid,
                        i64 from, i32 attended)
{
    Buf     b(&scratch, 256);
    JsonOut w = {};
    w.b = &b;
    putobject(&w);
    if (sid.len) {
        putstrkey(&w, "sid", sid);
    }
    putend(&w);
    JsonDoc d = jsonparse(&scratch, finish(&b));
    return hostforward(h, scratch, d.root, cmd, kind, from, attended);
}

// Ask for a snapshot of what streams for a session, and set whether it
// is attended, without replaying anything.
static void hostpoke(Host *h, Arena scratch, Str sid, i32 attended)
{
    hostown(h, scratch, CMD_OPEN, HOSTCMD_QUIET, sid, I64MAX, attended);
}


// Output to clients

// Room that live records leave in a client's queue for replies.
static iz hostreserve(HostClient *c)
{
    return c->link.keepmax / 32;
}

static b32 hostput(HostClient *c, Str line, iz reserve)
{
    return linkroom(&c->link)-(FRAME_HEAD+line.len)>=reserve && linksend(&c->link, line);
}

static void hostdrop(Host *, HostClient *, Arena scratch);

// A line a client must get: a reply. A client with no room left even
// for that is hopelessly behind, and loses its place. The scratch is
// what the caller has left free: below it may be the command at hand.
static void hostmust(Host *h, HostClient *c, Arena scratch, Str line)
{
    if (!c->gone && !hostput(c, line, 0)) {
        hostsay(h->os, scratch, "a client fell too far behind, so it starts over");
        hostdrop(h, c, scratch);
    }
}

static void hostreply(Host *h, HostClient *c, Arena scratch, i64 id, b32 ok,
                      Str err, Str sid)
{
    Buf   b(&scratch, 256 + err.len);
    EvOut w = evbegin(&b, sid.len ? sid : Str{}, 0, os_time(h->os), EV_REPLY);
    putintkey(&w.json, "id", id);
    putintkey(&w.json, "ok", ok);
    if (err.len) {
        putstrkey(&w.json, "err", err);
    }
    Str line = evend(&w);
    hostmust(h, c, scratch, line);
}

// An engine reply with the client's id in place of the host's.
static Str hostreid(Arena *a, Json *j, i64 cid)
{
    Buf     b(a, 1024);
    JsonOut w = {};
    w.b = &b;
    putobject(&w);
    for (Json *m = j->child; m; m = m->next) {
        putkey(&w, m->key);
        if (m->key == "id") {
            putint(&w, cid);
        } else {
            putjson(&w, m);
        }
    }
    putend(&w);
    putbyte(&b, '\n');
    return finish(&b);
}


// Clients

static HostClient *newclient(Host *h, u64 token)
{
    Region *r = os_reserve(h->os, HOST_CLIENTMEM);
    if (!r) {
        return 0;
    }
    Arena       a = newarena(r);
    HostClient *c = alloc<HostClient>(&a);
    c->region = r;
    c->token  = token;
    c->link   = newlink(&a, os_random(h->os), h->keepmax, HOST_KEEPCOUNT);
    c->perm   = a;
    c->next    = h->clients;
    h->clients = c;
    return c;
}

// A stream ends. Its memory goes at the end of the round (hostreap), as
// the command at hand may lie in its decoder's buffer.
static void connfree(Host *h, HostConn *hc)
{
    for (HostConn **p = &h->conns; *p; p = &(*p)->next) {
        if (*p == hc) {
            *p = hc->next;
            break;
        }
    }
    if (hc->client && hc->client->conn==hc) {
        hc->client->conn   = 0;
        hc->client->lostat = os_ticks(h->os);
    }
    os_connclose(h->os, hc->c);
    hc->next     = h->deadconns;
    h->deadconns = hc;
}

// A client leaves for good: what it opened stays open, but unattended
// unless another client attends it.
static void hostdrop(Host *h, HostClient *c, Arena scratch)
{
    if (c->gone) {
        return;
    }
    c->gone = 1;
    if (c->conn) {
        connfree(h, c->conn);
    }
    while (c->subs) {
        Arena a   = scratch;
        Str   sid = clone(&a, c->subs->sid);
        b32   att = c->subs->attended;
        hostunsub(h, c, a, sid);
        if (att && !hostattended(h, sid)) {
            hostpoke(h, a, sid, 0);
        }
    }
    for (i32 i = 0; i < h->cmdlen; i++) {
        HostCmd *q = hostcmdat(h, i);
        if (q->client == c) {
            q->client = 0;  // its reply goes nowhere
        }
    }
    for (HostClient **p = &h->clients; *p; p = &(*p)->next) {
        if (*p == c) {
            *p = c->next;
            break;
        }
    }
    c->next = h->dead;  // a caller may still hold it
    h->dead = c;
}

// Release the clients and streams dropped this round, now that nothing
// holds them.
static void hostreap(Host *h)
{
    while (h->dead) {
        HostClient *c = h->dead;
        h->dead = c->next;
        os_release(h->os, c->region);
    }
    while (h->deadconns) {
        HostConn *hc = h->deadconns;
        h->deadconns = hc->next;
        os_release(h->os, hc->region);
    }
}

// Remove a client's subscription, and those to the subagents under it.
static void hostunsub(Host *h, HostClient *c, Arena scratch, Str sid)
{
    for (HostSub **p = &c->subs; *p; p = &(*p)->next) {
        HostSub *s = *p;
        if (s->sid == sid) {
            *p       = s->next;
            s->next  = c->spare;
            c->spare = s;
            break;
        }
    }
    for (b32 again = 1; again;) {
        again = 0;
        for (HostSub *s = c->subs; s; s = s->next) {
            HostSess *ss = upsert(&h->sess, s->sid, (Arena *)0);
            if (ss && ss->parent==sid) {
                Arena a   = scratch;
                Str   kid = clone(&a, s->sid);  // its memory is reused
                hostunsub(h, c, a, kid);
                again = 1;
                break;
            }
        }
    }
}


// Commands from clients

// The host's hello, then the reply: refused when the client says it
// speaks another protocol (proto, the major version), so that both ends
// know.
static void hosthello(Host *h, HostClient *c, Arena scratch, i64 id, Json *j)
{
    Buf   b(&scratch, 512);
    EvOut w = evbegin(&b, {}, 0, os_time(h->os), EV_HELLO);
    putintkey(&w.json, "proto", h->proto);
    putstrkey(&w.json, "version", PROSPERO_VERSION);
    putcaps(&w.json, 1);
    putintkey(&w.json, "pid", (i64)os_pid(h->os));
    Str line = evend(&w);
    hostmust(h, c, scratch, line);
    if (c->gone) {
        return;  // too far behind for even that: dropped
    }

    i64 proto = jsonint(jsonget(j, "proto"), h->proto);
    Str err   = {};
    if (proto != h->proto) {
        Buf e(&scratch, 128);
        print(&e, "this host speaks protocol ");
        print(&e, (i64)h->proto);
        print(&e, " (version " PROSPERO_VERSION "), not ");
        print(&e, proto);
        err = finish(&e);
    }
    hostreply(h, c, scratch, id, !err.len, err, {});
}

static void hostcommand(Host *h, HostClient *c, Arena scratch, Str line)
{
    line = trim(line);
    JsonDoc d   = jsonparse(&scratch, line);
    Json   *j   = d.root;
    i64     cid = jsonint(jsonget(j, "id"), 0);
    if (!d.ok || j->type!=JSON_OBJECT) {
        hostreply(h, c, scratch, cid, 0, "a command must be a JSON object", {});
        return;
    }
    i32 cmd = cmdfind(jsonstr(jsonget(j, "cmd")));
    Str sid = jsonstr(jsonget(j, "sid"));
    HostCmd *q = 0;
    switch (cmd) {
    case CMD_HELLO:
        hosthello(h, c, scratch, cid, j);
        return;
    case CMD_OPEN: {
        if (!validsid(sid)) {
            hostreply(h, c, scratch, cid, 0, "invalid session id", {});
            return;
        }
        i64 from = jsonint(jsonget(j, "from"), 0);
        b32 att  = jsonflag(jsonget(j, "attended"), 0);
        q = hostforward(h, scratch, j, cmd, HOSTCMD_CLIENT, I64MAX,
                        att || hostattended(h, sid));
        if (q) {
            q->from     = from>0 ? from : 0;
            q->attended = att;
        }
    } break;
    case CMD_NEW:
        q = hostforward(h, scratch, j, cmd, HOSTCMD_CLIENT, -1, -1);
        if (q) {
            q->attended = jsonflag(jsonget(j, "attended"), 0);
        }
        break;
    case CMD_CLOSE: {
        HostSub *s = findsub(c, sid);
        if (!s) {
            hostreply(h, c, scratch, cid, 0, "no such open session", sid);
            return;
        }
        b32 att = s->attended;
        hostunsub(h, c, scratch, sid);
        if (hostopened(h, sid)) {
            // Others have it open: it stays so
            hostreply(h, c, scratch, cid, 1, {}, sid);
            if (att && !hostattended(h, sid)) {
                hostpoke(h, scratch, sid, 0);
            }
            return;
        }
        q = hostforward(h, scratch, j, cmd, HOSTCMD_CLIENT, -1, -1);
    } break;
    case CMD_QUIT:
        q = hostforward(h, scratch, j, cmd, HOSTCMD_CLIENT, -1, -1);
        h->quitting |= q != 0;
        break;
    default:
        q = hostforward(h, scratch, j, cmd, HOSTCMD_CLIENT, -1, -1);
    }
    if (!q) {
        hostreply(h, c, scratch, cid, 0, "too many commands are waiting", sid);
        return;
    }
    q->client = c;
    q->cid    = cid;
    if (sid.len <= countof(q->sid)) {
        copybytes(q->sid, sid.data, sid.len);
        q->sidlen = sid.len;
    }
}


// Records from the engine

// Give one session record to a client's subscription: in order, or else
// from the log. Returns 1 if it just fell behind for want of room.
static b32 hostroute(HostClient *c, HostSub *s, EvRec *r, Str line)
{
    if (!r->seq) {
        if (!s->catchup) {
            hostput(c, line, hostreserve(c));  // lost if no room: provisional
        }
        return 0;
    }
    if (s->catchup) {
        s->newest = r->seq>s->newest ? r->seq : s->newest;
        return 0;
    } else if (r->seq <= s->sent) {
        return 0;  // given already, from the log
    }
    b32 gap = r->seq > s->sent+1;
    if (gap || !hostput(c, line, hostreserve(c))) {
        s->catchup = 1;  // the log has it
        s->newest  = r->seq;
        return !gap;
    }
    s->sent = r->seq;
    return 0;
}

static void hostquiet(Host *h, Arena scratch, i64 now, b32 status);

static void hostreplied(Host *h, Arena scratch, EvRec *r, Str line)
{
    i64 id = jsonint(jsonget(r->root, "id"), 0);
    i64 ok = jsonint(jsonget(r->root, "ok"), 0);
    if (!h->cmdlen || id<hostcmdat(h, 0)->id) {
        return;  // not one of the host's
    }
    while (h->cmdlen) {
        HostCmd q = *hostcmdat(h, 0);
        if (q.id > id) {
            return;
        }
        h->cmdhead = (h->cmdhead + 1) % HOST_CMDS;
        h->cmdlen--;
        Str sid = {q.sid, q.sidlen};
        if (q.id < id) {
            // Its reply was lost to a full inbox, as replies come in order
            if (q.kind==HOSTCMD_CLIENT && q.client) {
                hostreply(h, q.client, scratch, q.cid, 0,
                          "the reply was lost: try again", sid);
            } else if (q.kind == HOSTCMD_STATUS) {
                h->checking = 0;
            }
            continue;
        }

        switch (q.kind) {
        case HOSTCMD_STATUS: {
            h->checking = 0;
            i64 now = os_ticks(h->os);
            i64 turns = jsonint(jsonget(r->root, "turns"), 0);
            i64 jobs  = jsonint(jsonget(r->root, "jobs"), 0);
            if (turns || jobs) {
                h->busyat = now;
            }
            hostquiet(h, scratch, now, 1);
        } return;
        case HOSTCMD_QUIET:
            return;
        }
        HostClient *c = q.client;
        if (!c) {
            return;
        }
        if (ok && q.cmd==CMD_OPEN) {
            hostsub(c, sid, q.from, q.attended);
        } else if (ok && q.cmd==CMD_NEW && validsid(r->sid)) {
            hostsub(c, r->sid, 0, q.attended);
        }
        Str out = hostreid(&scratch, r->root, q.cid);
        hostmust(h, c, scratch, out);
        return;
    }
}

static void hostrecord(Host *h, Arena scratch, Str line)
{
    EvRec r = evparse(&scratch, line);
    if (!r.ok) {
        return;
    } else if (r.ev == EV_REPLY) {
        hostreplied(h, scratch, &r, line);
        return;
    } else if (!r.sid.len) {
        // The sessions list: the asker's, the command at the front
        HostCmd *q = h->cmdlen ? hostcmdat(h, 0) : 0;
        if (q && q->client) {
            hostmust(h, q->client, scratch, line);
        }
        return;
    }

    HostSess *ss = hostsess(h, r.sid);
    if (r.ev == EV_LIVE) {
        ss->busy = !(jsonstr(jsonget(r.root, "state")) == "idle");
    } else if (r.ev==EV_SESSION && r.seq==1) {
        // A subagent's session: for whoever has its parent open
        Str parent = jsonstr(jsonget(r.root, "parent"));
        if (validsid(parent)) {
            ss->parent = clone(&h->perm, parent);
            for (HostClient *c = h->clients; c; c = c->next) {
                HostSub *p = findsub(c, parent);
                if (p && !findsub(c, r.sid)) {
                    HostSub *s = hostsub(c, r.sid, 0, 0);
                    s->catchup = p->catchup;
                }
            }
        }
    }
    for (HostClient *c = h->clients; c; c = c->next) {
        HostSub *s = findsub(c, r.sid);
        if (s && hostroute(c, s, &r, line)) {
            Arena a = scratch;
            hostsay(h->os, a, "a client fell behind, so its records come from "
                    "the log", r.sid);
        }
    }
}

// Feed a client what it is missing of a session, from the log, while its
// link has room for it, keeping half of it for what else comes, unless
// it is empty: a line of any size then goes, so that this always moves
// on. A line too long for any frame, or for the link, is skipped. Once
// all is given, it takes records live again.
static void hostcatchup(Host *h, HostClient *c, HostSub *s, Arena scratch)
{
    Str path = join(&scratch, join(&scratch, join(&scratch, h->statedir,
                    "sessions"), s->sid), "events.jsonl");
    iz  most = c->link.keepmax - FRAME_HEAD - hostreserve(c);
    most = most<FRAME_MAX ? most : FRAME_MAX;  // the same but in tests
    for (;;) {
        Link *l       = &c->link;
        iz    half    = l->keepmax / 2;
        iz    reserve = l->qcount ? half : hostreserve(c);
        if (linkroom(l) < reserve+FRAME_HEAD) {
            return;  // more as it drains
        }
        Arena a  = scratch;
        Read  rd = os_readat(h->os, &a, path, s->off, HOST_CHUNK);
        if (rd.err!=READ_OK && rd.err!=READ_NOTFOUND) {
            return;  // try again later
        }

        if (s->skip) {
            // Inside a line too long to send: on to its end, unseen
            Cut nl = cut(rd.data, '\n');
            s->off  += nl.ok ? nl.head.len+1 : rd.data.len;
            s->skip  = !nl.ok;
            if (rd.data.len) {
                continue;
            }
        } else {
            if (!cut(rd.data, '\n').ok && rd.data.len==HOST_CHUNK) {
                // A long line: whole, if a frame can carry it
                a  = scratch;
                rd = os_readat(h->os, &a, path, s->off, FRAME_MAX+1);
                if (rd.err != READ_OK) {
                    return;
                } else if (!cut(rd.data, '\n').ok && rd.data.len==FRAME_MAX+1) {
                    s->off  += rd.data.len;
                    s->skip  = 1;
                    continue;
                }
            }

            iz  used = 0;
            b32 full = 0;
            for (Str rest = rd.data;;) {
                Cut nl = cut(rest, '\n');
                if (!nl.ok) {
                    break;  // the end, or a torn tail being written
                }
                Arena t   = a;
                EvRec rec = evparse(&t, nl.head);
                Str   ln  = {nl.head.data, nl.head.len+1};
                if (rec.ok && rec.sid==s->sid && rec.seq>s->sent) {
                    if (ln.len > most) {
                        s->sent = rec.seq;  // cannot travel: skipped
                    } else if (hostput(c, ln, reserve)) {
                        s->sent = rec.seq;
                        reserve = half;
                    } else {
                        full = 1;
                        break;
                    }
                }
                used += ln.len;
                rest  = nl.tail;
            }
            s->off += used;
            if (full) {
                return;
            } else if (used) {
                continue;
            }
        }

        // At the end of the log: live from here on, and the state of a
        // session at work comes again, with what streams as a snapshot
        s->catchup = 0;
        if (s->newest > s->sent) {
            s->sent = s->newest;  // never logged (a full disk): let it go
        }
        HostSess *ss = upsert(&h->sess, s->sid, (Arena *)0);
        if (ss && ss->busy) {
            hostpoke(h, scratch, s->sid, -1);
        }
        return;
    }
}


// Connections and streams

static HostConn *findconn(Host *h, u64 id)
{
    for (HostConn *hc = h->conns; hc; hc = hc->next) {
        if (hc->id == id) {
            return hc;
        }
    }
    return 0;
}

static HostClient *findclient(Host *h, u64 token)
{
    for (HostClient *c = h->clients; c; c = c->next) {
        if (c->token == token) {
            return c;
        }
    }
    return 0;
}

static void hostframes(Host *h, HostConn *hc, Arena scratch, Str data)
{
    i64 now = os_ticks(h->os);
    if (hc->client && data.len) {
        linkheard(&hc->client->link, now);  // a frame in pieces counts too
    }
    for (FrameIn f; (f = framenext(&hc->dec, &data)).ok;) {
        HostClient *c = hc->client;
        if (!c) {
            // The first frame names the client
            if (f.type != FRAME_REPLAY) {
                connfree(h, hc);
                return;
            }
            u8 *p     = f.payload.data;
            u64 token = (u64)frameget32(p)<<32 | frameget32(p+4);
            c = findclient(h, token);
            c = c ? c : newclient(h, token);
            if (!c) {
                hostsay(h->os, scratch, "out of memory for a client");
                connfree(h, hc);
                return;
            }
            if (c->conn) {
                connfree(h, c->conn);  // replaced by this one
            }
            hc->client = c;
            c->conn    = hc;
            linkconnect(&c->link, now);
        }
        Arena a = scratch;
        switch (linkframe(&c->link, &f, now)) {
        case LINK_DATA:
            hostcommand(h, c, a, f.payload);
            if (c->gone) {
                return;  // hc is gone with it
            }
            break;
        case LINK_BYE:
            hostdrop(h, c, scratch);
            return;
        case LINK_BROKEN:
            connfree(h, hc);
            return;
        }
    }
    if (hc->dec.bad) {
        connfree(h, hc);
    }
}

// Write what each client's link has due to its stream.
static void hostflush(Host *h, Arena scratch, i64 now)
{
    for (HostConn *hc = h->conns, *next; hc; hc = next) {
        next = hc->next;
        HostClient *c = hc->client;
        if (!c) {
            continue;
        }
        Arena a   = scratch;
        Str   out = linkout(&c->link, &a, now);
        if (out.len && !os_connwrite(h->os, hc->c, out)) {
            connfree(h, hc);
        }
    }
}

// No client connected and nothing run for idle_exit: stop. While quiet,
// the engine's status is taken every HOST_CHECK, and once more at the
// deadline, and only a status just taken (status set) stops the host.
static void hostquiet(Host *h, Arena scratch, i64 now, b32 status)
{
    b32 any = 0;
    for (HostConn *hc = h->conns; hc && !any; hc = hc->next) {
        any = hc->client != 0;
    }
    if (any || h->quitting) {
        h->quietat = 0;
        h->checkat = 0;
        return;
    } else if (!h->quietat) {
        h->quietat = now;
        h->checkat = 0;
    }
    i64 since = h->quietat>h->busyat ? h->quietat : h->busyat;
    if (status && now-since>=h->idle) {
        hostsay(h->os, scratch, "idle, so exiting");
        hostown(h, scratch, CMD_QUIT, HOSTCMD_QUIET, {}, -1, -1);
        h->quitting = 1;
        return;
    } else if (h->checking || h->checkat) {
        return;  // a check is due already
    }
    i64 next = since + h->idle;
    h->checkat = next-now<HOST_CHECK ? next : now+HOST_CHECK;
}

static i64 hostdeadline(Host *h)
{
    i64 r = h->checkat ? h->checkat : I64MAX;
    for (HostConn *hc = h->conns; hc; hc = hc->next) {
        i64 t = hc->client ? linkdeadline(&hc->client->link) : hc->since+HOST_HELLO;
        r = t<r ? t : r;
    }
    for (HostClient *c = h->clients; c; c = c->next) {
        i64 t = c->conn ? I64MAX : c->lostat+h->grace;
        r = t<r ? t : r;
    }
    return r;
}

static void hosttimers(Host *h, i64 now)
{
    for (HostConn *hc = h->conns, *next; hc; hc = next) {
        next = hc->next;
        if (!hc->client && now-hc->since>=HOST_HELLO) {
            connfree(h, hc);  // never said who it is
        } else if (hc->client && !linktick(&hc->client->link, now)) {
            hostsay(h->os, newarena(h->scratch), "a client's stream went silent");
            connfree(h, hc);  // dead: its client may come back
        }
    }
    for (HostClient *c = h->clients, *next; c; c = next) {
        next = c->next;
        if (!c->conn && now-c->lostat>=h->grace) {
            hostdrop(h, c, newarena(h->scratch));
        }
    }
    if (h->checkat && now>=h->checkat && !h->quitting) {
        h->checkat  = 0;
        h->checking = hostown(h, newarena(h->scratch), CMD_STATUS, HOSTCMD_STATUS,
                              {}, -1, -1) != 0;
    }
}

// The engine stopped: goodbye to every client, after what it has due,
// then wait a moment for the streams to close, so that it all gets out.
static void hostbye(Host *h)
{
    Os *os  = h->os;
    i64 now = os_ticks(os);
    for (HostConn *hc = h->conns; hc; hc = hc->next) {
        if (hc->client) {
            linkbye(&hc->client->link);
        }
    }
    hostflush(h, newarena(h->scratch), now);
    for (HostConn *hc = h->conns; hc; hc = hc->next) {
        os_connwrite(os, hc->c, {});
    }
    i64 until = now + 2*SEC;
    while (h->conns && (now = os_ticks(os))<until) {
        mailwait(h->inbox, (i32)((until - now)/MS + 1));
        Mail m = mailtake(h->inbox);
        for (Record r; (r = mailnext(&m)).ok;) {
            HostConn *hc = r.type==CONN_EOF ? findconn(h, r.id) : 0;
            if (hc) {
                connfree(h, hc);
            } else if (r.type == CONN_NEW) {
                Conn *c = 0;
                copybytes(&c, r.data.data, sizeof(c));
                os_connclose(os, c);
            }
        }
    }
}

static i32 hostloop(Host *h)
{
    Os *os = h->os;
    hostquiet(h, newarena(h->scratch), os_ticks(os), 0);
    for (;;) {
        i64 now  = os_ticks(os);
        i64 next = hostdeadline(h);
        i32 ms   = -1;
        if (next != I64MAX) {
            i64 wait = (next - now + MS - 1) / MS;
            ms = wait<0 ? 0 : wait>3600000 ? 3600000 : (i32)wait;
        }
        if (ms) {
            mailwait(h->inbox, ms);
        }

        Mail m = mailtake(h->inbox);
        if (m.overflow) {
            // Engine records were lost: sessions catch up from their
            // logs, and a status flushes out replies that went missing
            for (HostClient *c = h->clients; c; c = c->next) {
                for (HostSub *s = c->subs; s; s = s->next) {
                    s->catchup = 1;
                }
            }
            hostown(h, newarena(h->scratch), CMD_STATUS, HOSTCMD_QUIET, {}, -1, -1);
        }
        b32 stopped = 0;
        for (Record r; (r = mailnext(&m)).ok;) {
            Arena scratch = newarena(h->scratch);
            switch (r.type) {
            case MAIL_OUT:
                hostrecord(h, scratch, r.data);
                break;
            case MAIL_QUIT:
                stopped = 1;
                break;
            case HOST_STOPALL:
                hostown(h, scratch, CMD_STOP, HOSTCMD_QUIET, {}, -1, -1);
                break;
            case HOST_QUITALL:
                if (!h->quitting) {
                    hostsay(os, scratch, "asked to stop");
                    hostown(h, scratch, CMD_QUIT, HOSTCMD_QUIET, {}, -1, -1);
                    h->quitting = 1;
                }
                break;
            case CONN_NEW: {
                Conn *c = 0;
                copybytes(&c, r.data.data, sizeof(c));
                Region *rg = os_reserve(os, FRAME_MAX + ((iz)1<<20));
                if (!rg) {
                    os_connclose(os, c);
                    break;
                }
                Arena     a  = newarena(rg);
                HostConn *hc = alloc<HostConn>(&a);
                hc->region = rg;
                hc->c      = c;
                hc->id     = r.id;
                hc->since  = os_ticks(os);
                hc->dec    = newframedec(&a, FRAME_MAX);
                hc->next   = h->conns;
                h->conns   = hc;
            } break;
            case CONN_DATA: {
                HostConn *hc = findconn(h, r.id);
                if (hc) {
                    hostframes(h, hc, scratch, r.data);
                }
            } break;
            case CONN_EOF: {
                HostConn *hc = findconn(h, r.id);
                if (hc) {
                    connfree(h, hc);
                }
            } break;
            }
        }

        now = os_ticks(os);
        hosttimers(h, now);
        for (HostClient *c = h->clients; c; c = c->next) {
            for (HostSub *s = c->subs; s && c->conn; s = s->next) {
                if (s->catchup) {
                    hostcatchup(h, c, s, newarena(h->scratch));
                }
            }
        }
        hostquiet(h, newarena(h->scratch), now, 0);
        if (stopped) {
            hostbye(h);
            hostreap(h);
            return 0;
        }
        hostflush(h, newarena(h->scratch), now);
        hostreap(h);
    }
}


// prospero serve

static Str serveusage =
"usage: prospero serve [options]\n"
"options:\n"
"  -c FILE       configuration file\n"
"  --state DIR   session storage directory\n"
"  --idle SECS   exit after this long with no client and no work\n"
"                (default: [host] idle_exit)\n";

// A setting in seconds, as ticks.
static i64 hostticks(Config *cfg, Str key)
{
    return cfgms(cfg, "host", "", key, 0) * MS;
}

// Take the lock that makes the host of a state directory the only one:
// its handle, or -1 if another process holds it.
static iz hostlock(Os *os, Arena *perm, Arena scratch, Str statedir)
{
    Str run = join(perm, statedir, "run");
    if (!os_mkdirs(os, scratch, run)) {
        return -1;
    }
    Str lock = concat(perm, join(perm, run, hostname(perm, HOST_PROTO)), ".lock");
    return os_lockfile(os, scratch, lock);
}

// A host for an engine that runs already, whose records arrive in its
// inbox, which has no notify hook: given the lock (hostlock, else -1 to
// take it here), the endpoint, the listener and the registration. The
// host's commands to the engine take ids above firstid, and so above
// those of whoever had the engine before, whose late replies are then
// never taken for the host's. The host owns perm from then on: nothing
// else may allocate there. Null on failure, with why in *err, and the
// lock let go.
struct HostConf {
    Config  *cfg;
    Str      statedir;
    Engine  *e;
    Mailbox *inbox;
    i64      idle;     // ticks
    i64      firstid;
    iz       held;     // the lock, or -1
};

static Host *newhost(Os *os, Arena *perm, Arena scratch, HostConf *hc, Str *err)
{
    Host *h = alloc<Host>(perm);
    h->os       = os;
    h->cfg      = hc->cfg;
    h->proto    = HOST_PROTO;
    h->statedir = hc->statedir;
    h->e        = hc->e;
    h->inbox    = hc->inbox;
    h->nextid   = hc->firstid>0 ? hc->firstid : 0;
    h->held     = hc->held>=0 ? hc->held : hostlock(os, perm, scratch, h->statedir);
    if (h->held < 0) {
        *err = concat(perm, "a host already runs for this state directory: ", h->statedir);
        return 0;
    }
    // Test hooks: another protocol, to test a mismatch, and a small
    // queue per client, to test one that falls behind
    Int proto = parseint(os_getenv(os, &scratch, "PROSPERO_TEST_PROTO"));
    Int keep  = parseint(os_getenv(os, &scratch, "PROSPERO_TEST_KEEPMAX"));
    h->proto   = proto.ok ? (i32)proto.val : h->proto;
    h->keepmax = HOST_KEEPMAX;
    if (keep.ok && keep.val>=1<<10 && keep.val<=HOST_KEEPMAX) {
        h->keepmax = (iz)keep.val;
    }

    Str why = {};
    h->endpoint = os_endpoint(os, perm, h->statedir, hostname(perm, HOST_PROTO));
    h->scratch  = os_reserve(os, HOST_SCRATCH);
    h->cmds     = alloc<HostCmd>(perm, HOST_CMDS);
    h->idle     = hc->idle;
    h->grace    = hostticks(h->cfg, "grace");
    h->busyat   = os_ticks(os);
    if (!h->endpoint.len) {
        why = concat(perm, "cannot make a place for the endpoint: ", os_lasterr(os, &scratch));
    } else if (!h->scratch) {
        why = "out of memory";
    } else if (!(h->listener = os_listen(os, scratch, h->endpoint, h->inbox))) {
        why = concat(perm, "cannot listen: ", os_lasterr(os, &scratch));
    }
    if (why.len) {
        *err = why;
        os_unlockfile(os, h->held);
        return 0;
    }

    // Registered, after clearing out what is stale: anything with this
    // endpoint (whose lock is held here), or that nobody answers
    Str dir = hostsdir(perm, h->statedir);
    os_mkdirs(os, scratch, dir);
    for (HostEntry *e = hostentries(os, &scratch, h->statedir); e; e = e->next) {
        Arena a = scratch;
        if (e->endpoint==h->endpoint || !os_listening(os, a, e->endpoint)) {
            os_remove(os, a, e->path);
        }
    }
    i64 pid = os_pid(os);
    Buf b(&scratch, 64);
    print(&b, pid);
    print(&b, ".ini");
    h->regpath = join(perm, dir, finish(&b));
    Str entry = hostentry(&scratch, pid, h->endpoint, os_time(os), HOST_PROTO);
    os_write(os, scratch, h->regpath, entry);

    b = Buf(&scratch, 256);
    print(&b, "host ");
    print(&b, pid);
    print(&b, " listening at ");
    print(&b, h->endpoint);
    hostsay(os, scratch, finish(&b));
    h->perm = *perm;  // for session names, from now on
    return h;
}

// What a host knows of a session before it starts, from whoever had its
// engine before: a subagent's parent, and whether it is at work. Only
// before hostrun, on the thread that will run it.
static void hostseed(Host *h, Str sid, Str parent, b32 busy)
{
    HostSess *ss = hostsess(h, sid);
    ss->busy = busy;
    if (validsid(parent) && !ss->parent.len) {
        ss->parent = clone(&h->perm, parent);
    }
}

// From any thread: stop every turn and job, the host running on (quit
// clear), or stop the engine, and with it the host (quit set).
static void hoststop(Host *h, b32 quit)
{
    mailpost(h->inbox, quit ? HOST_QUITALL : HOST_STOPALL, 0, {}, 1);
}

// Run a host until its engine stops, then take down its place: the
// listener, the registration and the lock.
static i32 hostrun(Host *h)
{
    Os   *os      = h->os;
    i32   r       = hostloop(h);
    Arena scratch = newarena(h->scratch);
    os_unlisten(os, h->listener);
    os_remove(os, scratch, h->regpath);
    os_unlockfile(os, h->held);
    hostsay(os, scratch, "host stopped");
    return r;
}

static i32 servemain(Os *os, Arena perm, Arena scratch, Str *args, i32 nargs)
{
    Str config = {}, state = {}, idle = {};
    for (i32 i = 2; i < nargs; i++) {
        Str a    = args[i];
        b32 more = i+1 < nargs;
        if (a=="-c" && more) {
            config = args[++i];
        } else if (a=="--state" && more) {
            state = args[++i];
        } else if (a=="--idle" && more) {
            idle = args[++i];
        } else {
            os_print(os, 2, serveusage);
            return 2;
        }
    }
    Float secs = parsefloat(idle);
    if (idle.len && (!secs.ok || secs.val<=0)) {
        os_print(os, 2, "prospero: --idle must be a number of seconds above 0\n");
        return 2;
    }
    // The lock first, so that a second host never starts an engine, nor
    // even reads the configuration
    Str      statedir = state.len ? state : defaultstate(os, &perm);
    HostConf hc       = {};
    hc.statedir = statedir;
    hc.held     = hostlock(os, &perm, scratch, statedir);
    if (hc.held < 0) {
        hostsay(os, scratch, "a host already runs for this state directory", statedir);
        return 1;
    }

    // URL includes without a cached copy come later, from the engine:
    // whoever started this host waits only so long for it to listen
    Config *cfg = cliconfig(os, &perm, scratch, config, statedir, CFG_NETLATER);
    if (!cfg) {
        return 2;
    }
    hc.cfg = cfg;
    hc.inbox = newmailbox(&perm, HOST_INBOX, 0, 0);
    // Asked to quit by a signal, it quits as when a client asks, taking
    // its endpoint and registration with it: before any thread starts
    if (hc.inbox && !os_onquit(os, hc.inbox, HOST_QUITALL)) {
        hostsay(os, scratch, "cannot watch for signals to quit");
    }
    hc.idle  = idle.len ? (i64)(secs.val*(f64)SEC) : hostticks(cfg, "idle_exit");
    hc.e     = hc.inbox ? newengine(os, &perm, cfg, statedir, hc.inbox) : 0;
    if (!hc.e || !enginestart(hc.e)) {
        hostsay(os, scratch, "cannot start the engine");
        return 2;
    }
    Str   err = {};
    Host *h   = newhost(os, &perm, scratch, &hc, &err);
    if (!h) {
        hostsay(os, scratch, err);
        return 1;
    }
    return hostrun(h);
}


// Starting a host, and prospero attach

struct HostStart {
    Conn *c;
    Str   err;       // why there is no connection
    b32   attached;  // started in this process's job: it ends with it
};

// Connect to the host of a state directory, starting one detached if
// none runs, and waiting up to HOST_READY for it to listen.
static HostStart hoststart(Os *os, Arena *perm, Arena scratch, Str statedir,
                           Str config, Str idle, Str endpoint, Mailbox *mb, u64 id)
{
    HostStart r = {};
    r.c = os_connect(os, scratch, endpoint, mb, id);
    if (r.c) {
        return r;
    }

    // Absolute paths: the host starts in the state directory
    Str state = os_realpath(os, &scratch, statedir);
    Str conf  = config.len ? os_realpath(os, &scratch, config) : Str{};
    if (!state.len || (config.len && !conf.len)) {
        r.err = clone(perm, state.len ? Str("cannot find the configuration file")
                                      : Str("cannot find the state directory"));
        return r;
    }
    Str        exe  = os_exepath(os, &scratch);
    Slice<Str> argv = {};
    argv = push(&scratch, argv, exe.len ? exe : Str("prospero"));
    argv = push(&scratch, argv, Str("serve"));
    argv = push(&scratch, argv, Str("--state"));
    argv = push(&scratch, argv, state);
    if (conf.len) {
        argv = push(&scratch, argv, Str("-c"));
        argv = push(&scratch, argv, conf);
    }
    if (idle.len) {
        argv = push(&scratch, argv, Str("--idle"));
        argv = push(&scratch, argv, idle);
    }
    Str log = join(&scratch, join(&scratch, state, "run"), "host.log");
    if (os_stat(os, scratch, log).size > HOST_LOGMAX) {
        os_remove(os, scratch, log);  // each host appends: start over
    }
    Detached d = os_detach(os, scratch, argv, state, log);
    if (!d.pid) {
        Str why = os_lasterr(os, &scratch);
        r.err = concat(perm, "cannot start a host: ", why);
        return r;
    }
    r.attached = d.attached;

    for (i64 until = os_ticks(os) + HOST_READY;;) {
        mailwait(mb, 20);  // nothing posts yet: a nap
        r.c = os_connect(os, scratch, endpoint, mb, id);
        if (r.c) {
            return r;
        } else if (os_ticks(os) >= until) {
            break;
        }
    }
    // What the last try said (another user's pipe there, say), and why
    // the host itself gave up, in its log
    Str why = os_lasterr(os, &scratch);
    Buf b(perm, 128 + why.len + log.len);
    print(&b, "the host did not start (connecting: ");
    print(&b, why);
    print(&b, "): see ");
    print(&b, log);
    r.err = finish(&b);
    return r;
}

enum : u32 {
    ATTACH_EOF = 200,  // standard input ended
};

struct AttachIn {
    Os      *os;
    Conn    *c;
    Mailbox *mb;
    u8      *buf;
    iz       cap;
};

// Standard input to the host, until either ends.
static void attachstdin(void *arg)
{
    AttachIn *a = (AttachIn *)arg;
    for (;;) {
        iz n = os_stdin(a->os, a->buf, a->cap);
        if (n<=0 || !os_connwrite(a->os, a->c, {a->buf, n})) {
            break;
        }
    }
    os_connwrite(a->os, a->c, {});
    mailpost(a->mb, ATTACH_EOF, 0, {}, 1);
}

static Str attachusage =
"usage: prospero attach [--reconnect] [options]\n"
"Bridge standard I/O to the host of a state directory, starting one\n"
"if none runs, or with --reconnect exiting with status 90 instead.\n"
"options:\n"
"  -c FILE       configuration file, for a host started\n"
"  --state DIR   session storage directory\n"
"  --idle SECS   idle exit of a host started\n";

static i32 attachmain(Os *os, Arena perm, Arena scratch, Str *args, i32 nargs)
{
    Str config = {}, state = {}, idle = {};
    b32 reconnect = 0;
    for (i32 i = 2; i < nargs; i++) {
        Str a    = args[i];
        b32 more = i+1 < nargs;
        if (a == "--reconnect") {
            reconnect = 1;
        } else if (a=="-c" && more) {
            config = args[++i];
        } else if (a=="--state" && more) {
            state = args[++i];
        } else if (a=="--idle" && more) {
            idle = args[++i];
        } else {
            os_print(os, 2, attachusage);
            return 2;
        }
    }
    state = state.len ? state : defaultstate(os, &perm);
    Str run = join(&perm, state, "run");
    if (!os_mkdirs(os, scratch, run)) {
        Str err = os_lasterr(os, &scratch);
        hostsay(os, scratch, "cannot create the state directory", err);
        return 1;
    }
    Str endpoint = os_endpoint(os, &perm, state, hostname(&perm, HOST_PROTO));
    if (!endpoint.len) {
        Str err = os_lasterr(os, &scratch);
        hostsay(os, scratch, "cannot find the endpoint", err);
        return 1;
    }

    Mailbox *mb = newmailbox(&perm, 1<<20, 0, 0);
    Conn    *c  = mb ? os_connect(os, scratch, endpoint, mb, 1) : 0;
    if (!mb) {
        hostsay(os, scratch, "out of memory");
        return 1;
    } else if (!c && reconnect) {
        hostsay(os, scratch, "no host is running");
        return HOST_NOSERVER;
    } else if (!c) {
        HostStart hs = hoststart(os, &perm, scratch, state, config, idle,
                                 endpoint, mb, 1);
        if (hs.attached) {
            hostsay(os, scratch, "this session's job forbids breaking away, so the "
                    "host runs in it, attached, and stops when the session ends");
        }
        if (!hs.c) {
            hostsay(os, scratch, hs.err);
            return 1;
        }
        c = hs.c;
    }

    AttachIn *in = alloc<AttachIn>(&perm);
    in->os  = os;
    in->c   = c;
    in->mb  = mb;
    in->cap = 1<<16;
    in->buf = allocbytes(&perm, in->cap);
    if (!os_thread(os, attachstdin, in)) {
        hostsay(os, scratch, "cannot read standard input");
        return 1;
    }
    i64 eofat = 0;
    for (;;) {
        i32 ms = -1;
        if (eofat) {
            i64 left = eofat - os_ticks(os);
            if (left <= 0) {
                return 0;  // the host never closed its end
            }
            ms = (i32)(left/MS + 1);
        }
        mailwait(mb, ms);
        Mail m = mailtake(mb);
        for (Record r; (r = mailnext(&m)).ok;) {
            switch (r.type) {
            case CONN_DATA:
                if (!os_print(os, 1, r.data)) {
                    return 1;  // nobody reads: gone
                }
                break;
            case CONN_EOF:
                return 0;
            case ATTACH_EOF:
                eofat = os_ticks(os) + 2*SEC;  // for the host to close
                break;
            }
        }
    }
}
