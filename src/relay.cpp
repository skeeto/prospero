// Thin engines: model calls relayed through the computers connected
//
// An engine whose own configuration defines no endpoint (cfgthin) is
// thin: it uses the models of the computers connected to it, which
// offer their settings (relay.offer) but never their URLs or keys. Each
// offer becomes a layer of the configuration under the user's file
// (cfgload), its endpoints at relay://NAME (relay+anthropic://NAME for
// kind anthropic), and a request to one of those goes to a computer
// offering it as a relay record, which that computer makes with its own
// key and answers part by part (relay.data). Records arrive for it just
// as os_http's do, so a session cannot tell, but for its new failures:
// lost, when the computer goes or a hop overflows, and nobase.
//
// A session's turn body goes as a delta against its last one to the same
// computer (delta.cpp), deflated when that is smaller, as do the bodies
// of llama.cpp's control calls and listings, which go whole. Parts of at
// most RELAY_PART go one at a time, as the host makes room for each
// (relay.more), so that a slow link slows a call rather than fails it.
//
// The serving side is any engine: relay.call is a thin engine's call,
// passed on by its client, which this engine makes at one of its own
// endpoints as its own sessions' calls are made, admitted in their line,
// and answers with relay.data records, held back while its client's
// inbox is too full for them (mailroom).
//
// A thin engine whose [web] search is not set searches the same way,
// through a computer that offers to: a call to the path "search" with
// the query, which that computer runs with its own [web] settings and
// answers with the tool's result, as JSON, for the agent to read.
//
// This is free and unencumbered software released into the public domain.

enum : i32 {
    RELAY_PART    = 512 << 10,  // decoded bytes per part, either way
    RELAY_PEERS   = 8,          // offers kept
    RELAY_LAYER   = 256 << 10,  // an offer's text as a layer
    RELAY_HOLD    = 8 << 20,    // a served call's output held back
    RELAY_MODELS  = 4096,       // model ids an endpoint offers
    RELAY_RETRY   = 5,          // ms between tries while held back
};

static iz relaybodymax()
{
    return (iz)(sizeof(void *)==8 ? 64 : 32) << 20;
}

static iz relaybasemax()  // bytes of bases kept, each side
{
    return (iz)(sizeof(void *)==8 ? 64 : 16) << 20;
}

struct RelayEp {
    Str        name;
    b32        anthropic;
    Slice<Str> models;  // as its listing gave them, none for any
};

// A connected computer's offer, kept after it goes, so that the sessions
// it relayed for keep their settings until another offers them.
struct RelayPeer {
    RelayPeer     *next;
    Region        *mem;      // its offer's, replaced whole by the next
    i64            peer;     // the host's number for the client
    i64            seq;      // offers before it: the newest wins
    Str            machine;  // at most 63 bytes, of [A-Za-z0-9._-]
    Str            layer;    // its configuration layer, INI
    Slice<RelayEp> eps;
    b32            search;   // it runs web searches ([web] search)
    Str            own;      // relays only sessions made with this token
    b32            away;     // no stream now: chosen by none
    b32            gone;
};

// A body kept as the base of the next one's delta: the thin side's last
// turn body for a session, with the computer it went to; the serving
// side's last for a session of an origin.
struct RelayBase {
    RelayBase *next;
    Region    *mem;
    Str        key;
    i64        peer;
    u64        hash;
    Str        body;
    i64        used;   // when last used: the least recent goes first
};

// A thin engine's relayed call.
struct RelayCall {
    RelayCall *next;
    Region    *mem;
    u64        id;       // its records', the relay record's rid
    i64        peer;
    Str        sid;      // the session's, null if none
    Str        endpoint;
    Str        path;     // after the endpoint's URL, as "v1/messages"
    Str        method;
    Str        accept;
    Str        model;
    i32        readms;
    b32        anthropic;
    b32        keep;     // a turn body, the base of the next
    u64        base;     // the base's hash, with data a delta, else 0
    iz         len;      // of the body
    iz         size;     // of the ops, or the body sent whole
    iz         zlen;     // deflated, 0 if not
    Str        data;     // what the parts carry
    i32        parts;
    i32        sent;     // parts emitted
    i32        room;     // parts the host made room for, the first included
    b32        tell;     // its computer is to hear that it was cancelled
    i32        err;      // with failing, the HTTP_DONE it ends with
    Str        why;      // in whybuf
    u8         whybuf[256];
    b32        failing;
    b32        ended;    // its HTTP_DONE was delivered
    b32        freed;    // and its handle freed: it goes
};

// A call made for a thin engine.
struct RelayServe {
    RelayServe *next;
    Region     *mem;
    Arena       arena;
    i64         cid;      // the client's number for it
    Str         origin;   // the client's name for the thin engine
    HostLink   *far;      // or the link it came by (hostlink.cpp)
    b32         dropped;  // whose end is gone: nothing more is sent
    Str         session;
    Endpoint    ep;
    Str         path;
    Str         method;
    Str         accept;
    Str         model;
    i32         readms;
    b32         keep;
    u64         base;
    iz          len;
    iz          size;
    iz          zlen;
    i32         parts;
    i32         got;
    Buf         data;     // the parts so far
    Str         body;
    b32         admits;   // waits its turn at the endpoint (admission)
    AdmPoint   *point;
    i32         slot;     // SLOT_*
    Str         slotmodel;
    u64         admid;
    i32         admepoch;
    i32         qpos;     // its place as last told, and the line's length
    i32         qlen;
    b32         go;       // admitted: send at the next pass
    Http       *http;
    u64         httpid;
    i32         status;
    Buf         errbody;  // an error's first bytes, for the KV-full check
    // What waits to be sent back, in this order
    b32         queued;
    b32         sent;
    b32         head;
    Str         headers;
    u8         *hold;     // body bytes, RELAY_HOLD
    iz          held;
    b32         done;
    Str         err;      // done's, "ok" or another
    Str         text;
    b32         finished; // done was sent: it goes once its request has
    b32         search;   // a web search, run with this engine's [web]
    WebRun     *web;      // which runs now
    i64         webby;    // until this deadline (ticks)
};

struct Relay {
    iz          part;     // RELAY_PART, smaller for a test (newengine)
    RelayPeer  *peers;
    i64         seq;
    RelayCall  *calls;    // in order of starting
    RelayServe *serves;
    RelayBase  *bases;    // both sides', apart by key
    iz          basebytes;
    i64         baseuse;
    b32         held;     // records wait for room in the out mailbox
    b32         farheld;  // or in a hostlink's
    CfgLayer    layers[RELAY_PEERS];
};


// Parts are smaller with PROSPERO_TEST_RELAY_PART, a test hook.
static Relay *newrelay(Os *os, Arena *perm)
{
    Relay *r    = alloc<Relay>(perm);
    Int    part = parseint(os_getenv(os, perm, "PROSPERO_TEST_RELAY_PART"));
    r->part = part.ok && part.val>=64 && part.val<RELAY_PART ? (iz)part.val : (iz)RELAY_PART;
    return r;
}


// Peers and offers

static RelayPeer *relaypeerbynum(Engine *e, i64 peer)
{
    for (RelayPeer *p = e->relay->peers; p; p = p->next) {
        if (p->peer == peer) {
            return p;
        }
    }
    return 0;
}

// A machine's name as a thin engine shows it: [A-Za-z0-9._-] kept, at
// most 63 bytes, since live builds within a small fixed buffer.
static Str relaymachine(Arena *a, Str s)
{
    Buf b(a, 64);
    for (iz i = 0; i<s.len && b.len<63; i++) {
        u8 c = s[i];
        if ((c>='a' && c<='z') || (c>='A' && c<='Z') || digit(c) ||
                c=='.' || c=='_' || c=='-') {
            putbyte(&b, c);
        }
    }
    return b.len ? finish(&b) : Str("another computer");
}

// A value as an offer's layer may carry it: printable, on one line.
static b32 relayvalue(Str v)
{
    if (v.len > 1024) {
        return 0;
    }
    for (iz i = 0; i < v.len; i++) {
        if (v[i]<0x20 && v[i]!='\t') {
            return 0;
        }
    }
    return 1;
}

// A model's name as a section header may hold it.
static b32 relaymodelname(Str name)
{
    b32 ok = name.len && name.len<=200;
    for (iz i = 0; ok && i < name.len; i++) {
        u8 c = name[i];
        ok = c>0x20 && c!=0x7f && c!='[' && c!=']' && c!='"' && c!=';' &&
             c!='#';
    }
    return ok;
}

// A [model] key an offer may set: the schema's, but like (flattened
// already) and max_concurrent (the serving engine's), and template.NAME.
static b32 relaymodelkey(Str key)
{
    if (key=="like" || key=="max_concurrent") {
        return 0;
    } else if (cfgtemplatename(key).len) {
        b32 ok = key.len <= 128;
        for (iz i = 0; ok && i < key.len; i++) {
            u8 c = key[i];
            ok = (c>='a' && c<='z') || (c>='A' && c<='Z') || digit(c) ||
                 c=='.' || c=='_' || c=='-';
        }
        return ok;
    }
    CfgKey *k = cfgkey("model", key);
    return k && !cfgfamily(k);
}

// A hostlink's token (farlink): the sessions it made, which alone its
// offer is for (own), know it from their new {task}.
static b32 relaytoken(Str s)
{
    b32 ok = s.len>0 && s.len<=32;
    for (iz i = 0; ok && i < s.len; i++) {
        ok = digit(s[i]) || (s[i]>='a' && s[i]<='f');
    }
    return ok;
}

static Str const relayepkeys[] = {
    "first_byte_timeout", "idle_timeout", "progress_timeout",
    "slow_retry", "slow_retry_budget",
};

// An offer (the offer command's) as a configuration layer, with what
// choosing a computer needs, in a. Anything unsound is left out.
static void relayparse(RelayPeer *p, Arena *a, Json *o)
{
    p->machine = relaymachine(a, jsonstr(jsonget(o, "machine")));
    p->search  = jsonflag(jsonget(o, "search"), 0);
    Str own    = jsonstr(jsonget(o, "own"));
    p->own     = relaytoken(own) ? clone(a, own) : Str{};

    Buf b(a, 4096);
    print(&b, "# relayed from ");
    print(&b, p->machine);
    putbyte(&b, '\n');
    Str dep = jsonstr(jsonget(o, "endpoint"));
    Str dmo = jsonstr(jsonget(o, "model"));

    Buf eps(a, 1024);
    for (Json *x = jsonget(o, "endpoints")->child; x; x = x->next) {
        Str  name = jsonstr(jsonget(x, "name"));
        b32  an   = jsonstr(jsonget(x, "kind")) == "anthropic";
        b32  dup  = 0;
        for (iz i = 0; i < p->eps.len; i++) {
            dup |= p->eps[i].name == name;
        }
        if (!relayname(name) || dup) {
            continue;
        }
        RelayEp ep = {};
        ep.name      = clone(a, name);
        ep.anthropic = an;
        i32 n = 0;
        for (Json *m = jsonget(x, "models")->child; m && n<RELAY_MODELS; m = m->next) {
            Str id = jsonstr(m);
            if (id.len && id.len<=256 && relayvalue(id)) {
                ep.models = push(a, ep.models, clone(a, id));
                n++;
            }
        }
        p->eps = push(a, p->eps, ep);

        print(&eps, "[endpoint ");
        print(&eps, name);
        print(&eps, an ? Str("]\nurl = relay+anthropic://") : Str("]\nurl = relay://"));
        print(&eps, name);
        print(&eps, an ? Str("\nkind = anthropic\n") : Str("\nkind = openai\n"));
        for (iz i = 0; i < countof(relayepkeys); i++) {
            Str v = trim(jsonstr(jsonget(x, relayepkeys[i])));
            if (v.len && relayvalue(v)) {
                print(&eps, relayepkeys[i]);
                print(&eps, " = ");
                print(&eps, v);
                putbyte(&eps, '\n');
            }
        }
    }

    // The defaults first, as the top level must be
    for (iz i = 0; i < p->eps.len; i++) {
        if (p->eps[i].name == dep) {
            print(&b, "endpoint = ");
            print(&b, dep);
            putbyte(&b, '\n');
        }
    }
    if (dmo.len && relayvalue(dmo) && !cut(dmo, '"').ok) {
        print(&b, "model = ");
        print(&b, dmo);
        putbyte(&b, '\n');
    }
    print(&b, finish(&eps));

    for (Json *x = jsonget(o, "models")->child; x; x = x->next) {
        Str name = jsonstr(jsonget(x, "name"));
        if (!relaymodelname(name) || b.len>RELAY_LAYER) {
            continue;
        }
        print(&b, "[model ");
        print(&b, name);
        print(&b, "]\n");
        for (Json *k = jsonget(x, "settings")->child; k; k = k->next) {
            Str v = jsonstr(k);
            if (relaymodelkey(k->key) && relayvalue(v)) {
                print(&b, k->key);
                print(&b, " = ");
                print(&b, v);
                putbyte(&b, '\n');
            }
        }
    }
    p->layer = finish(&b);
    if (p->layer.len > RELAY_LAYER) {
        p->layer = {};  // too much to take: offers nothing
        p->eps   = {};
    }
}

// Load the configuration again with the offers as they are now, oldest
// first, so that the newest definitions win, under the user's file.
static void relayload(Engine *e, Arena scratch)
{
    Relay *r = e->relay;
    i32    n = 0;
    for (i64 last = 0;;) {
        RelayPeer *next = 0;
        for (RelayPeer *p = r->peers; p; p = p->next) {
            if (p->seq>last && (!next || p->seq<next->seq)) {
                next = p;
            }
        }
        if (!next) {
            break;
        }
        last = next->seq;
        if (next->layer.len) {
            r->layers[n].name = concat(&scratch, "relayed from ", next->machine);
            r->layers[n].text = next->layer;
            n++;
        }
    }
    e->cfg->layers = {r->layers, n};
    cfgload(e->cfg, scratch);
    e->cfg->layers = {};  // the next load as offers then are, or none
    cfgapply(e, scratch);
}

static void relaywake(Engine *e, Arena scratch)
{
    for (Session *s = e->sessions; s; s = s->next) {
        if (s->state == S_RELAY) {
            proceed(s, scratch);
        }
    }
}

static void relayreply(Engine *e, Arena scratch, i64 id, b32 thin)
{
    Buf   b(&scratch, 256);
    EvOut w = evbegin(&b, {}, 0, os_time(e->os), EV_REPLY);
    putintkey(&w.json, "id", id);
    putintkey(&w.json, "ok", 1);
    putintkey(&w.json, "thin", thin);
    emit(e, evend(&w));
}

static void relayfailpeer(Engine *e, RelayPeer *p, Str why);

// relay.offer {peer, offer}: a client's offer, the host's peer number
// stamped on it, kept in place of its last. The reply's thin says
// whether this engine uses offers at all.
static void relayoffer(Engine *e, Arena scratch, i64 id, Json *j)
{
    Relay *r    = e->relay;
    i64    peer = jsonint(jsonget(j, "peer"), 0);
    Json  *o    = jsonget(j, "offer");
    if (peer<=0 || o->type!=JSON_OBJECT) {
        reply(e, scratch, id, 0, "relay.offer takes a peer and an offer", {});
        return;
    }
    Region *mem = os_reserve(e->os, (iz)8<<20);
    if (!mem) {
        reply(e, scratch, id, 0, "out of memory", {});
        return;
    }
    RelayPeer *p   = relaypeerbynum(e, peer);
    Arena      a   = newarena(mem);
    RelayPeer  got = {};
    relayparse(&got, &a, o);
    b32 changed = !p || !(p->layer == got.layer);
    if (!p) {
        // A new computer: the oldest kept goes if there are too many,
        // one that went before one still here
        i32 n = 0;
        for (RelayPeer *q = r->peers; q; q = q->next) {
            n++;
        }
        while (n-- >= RELAY_PEERS) {
            RelayPeer **old = 0;
            for (RelayPeer **q = &r->peers; *q; q = &(*q)->next) {
                b32 older = !old || ((*q)->gone==(*old)->gone ?
                            (*q)->seq<(*old)->seq : (*q)->gone);
                old = older ? q : old;
            }
            RelayPeer *dead = *old;
            *old = dead->next;
            relayfailpeer(e, dead, "it was forgotten");
            os_release(e->os, dead->mem);
        }
        p = alloc<RelayPeer>(&e->perm);  // peers are few, and reused
        p->next  = r->peers;
        r->peers = p;
    } else {
        os_release(e->os, p->mem);
    }
    RelayPeer *next = p->next;
    *p = got;
    p->next = next;
    p->mem  = mem;
    p->peer = peer;
    p->seq  = ++r->seq;
    if (changed) {
        relayload(e, scratch);
    }
    relayreply(e, scratch, id, e->cfg->thin);
    relaywake(e, scratch);
}


// Bases for deltas

static RelayBase *relaybase(Engine *e, Str key)
{
    for (RelayBase *b = e->relay->bases; b; b = b->next) {
        if (b->key == key) {
            b->used = ++e->relay->baseuse;
            return b;
        }
    }
    return 0;
}

static void relaydropbase(Engine *e, Str key)
{
    Relay *r = e->relay;
    for (RelayBase **b = &r->bases; *b; b = &(*b)->next) {
        if ((*b)->key == key) {
            RelayBase *dead = *b;
            *b = dead->next;
            r->basebytes -= dead->body.len;
            os_release(e->os, dead->mem);
            return;
        }
    }
}

// Keep body as key's base, the least recently used going while there
// is too much. A body too large to keep leaves key with none.
static void relaykeep(Engine *e, Str key, i64 peer, Str body)
{
    Relay *r = e->relay;
    relaydropbase(e, key);
    if (body.len > relaybasemax()) {
        return;
    }
    while (r->bases && r->basebytes+body.len>relaybasemax()) {
        RelayBase **old = &r->bases;
        for (RelayBase **b = &r->bases; *b; b = &(*b)->next) {
            old = (*b)->used<(*old)->used ? b : old;
        }
        relaydropbase(e, (*old)->key);
    }
    Region *mem = os_reserve(e->os, body.len + key.len + 4096);
    if (!mem) {
        return;
    }
    Arena      a = newarena(mem);
    RelayBase *b = alloc<RelayBase>(&a);
    b->mem  = mem;
    b->key  = clone(&a, key);
    b->peer = peer;
    b->hash = hash(body);
    b->body = clone(&a, body);
    b->used = ++r->baseuse;
    b->next = r->bases;
    r->bases = b;
    r->basebytes += body.len;
}


// The thin side's calls

// A computer's offer of ep, by name and kind, or null.
static RelayEp *relayepof(RelayPeer *p, Endpoint *ep)
{
    b32 an = ep->kind == EP_ANTHROPIC;
    for (iz i = 0; i < p->eps.len; i++) {
        RelayEp *x = &p->eps[i];
        if (x->name==ep->relay && x->anthropic==an) {
            return x;
        }
    }
    return 0;
}

// Whether a computer's offer of ep lists the model, or lists none.
static b32 relaylists(RelayPeer *p, Endpoint *ep, Str model)
{
    RelayEp *x   = relayepof(p, ep);
    b32      has = !x->models.len || !model.len;
    for (iz k = 0; !has && k < x->models.len; k++) {
        has = x->models[k] == model;
    }
    return has;
}

// Whether a computer may relay the session's request at ep: here, not
// away, and offering the endpoint by name with its kind. Its list of
// models does not decide it, since an endpoint may serve names it does
// not list (an alias, or llama-server, which lists its file).
static b32 relayeligible(RelayPeer *p, Endpoint *ep, Str own)
{
    return !p->away && !p->gone && (!p->own.len || p->own==own) &&
           relayepof(p, ep);
}

// A listing of the models that computers here offer at ep, as /v1/models
// gives them, into b. Returns 0 if none offers ep.
static b32 relaylisting(Engine *e, Buf *b, Endpoint *ep)
{
    b32 any = 0;
    b32 an  = ep->kind == EP_ANTHROPIC;
    JsonOut w = {b};
    putobject(&w);
    putkey(&w, "data");
    putarray(&w);
    Slice<Str> seen = {};
    for (RelayPeer *p = e->relay->peers; p; p = p->next) {
        for (iz i = 0; !p->gone && i < p->eps.len; i++) {
            RelayEp *x = &p->eps[i];
            if (!(x->name==ep->relay) || x->anthropic!=an) {
                continue;
            }
            any = 1;
            for (iz k = 0; k < x->models.len; k++) {
                b32 dup = 0;
                for (iz n = 0; !dup && n < seen.len; n++) {
                    dup = seen[n] == x->models[k];
                }
                if (!dup) {
                    seen = push(b->a, seen, x->models[k]);
                    putobject(&w);
                    putstrkey(&w, "id", x->models[k]);
                    putend(&w);
                }
            }
        }
    }
    putend(&w);
    putend(&w);
    return any;
}

// Whether a connected computer offers to search.
static b32 relaysearches(Engine *e)
{
    for (RelayPeer *p = e->relay->peers; p; p = p->next) {
        if (p->search && !p->gone) {
            return 1;
        }
    }
    return 0;
}

// The computer to run a web search for session s (or none): the one its
// model calls went to last, if it searches and may, else the newest
// offer that does. 0 if none.
static i64 relaysearcher(Engine *e, Session *s)
{
    Str        own  = s ? s->rrelay : Str{};
    i64        last = s ? s->relaypeer : 0;
    RelayPeer *best = 0;
    for (RelayPeer *p = e->relay->peers; p; p = p->next) {
        b32 may = p->search && !p->away && !p->gone && (!p->own.len || p->own==own);
        if (may && (!best || p->peer==last || (best->peer!=last && p->seq>best->seq))) {
            best = p;
        }
    }
    return best ? best->peer : 0;
}

// Choose the computer for the session's next request at ep: the one it
// used last, if it still may, else the newest offer that may and lists
// the model, else the newest that may. Returns 0 if none may.
static b32 relaychoose(Session *s, Endpoint *ep)
{
    Engine    *e     = s->e;
    Str        model = reqmc(s)->name;
    RelayPeer *best  = 0;
    i32        rank  = 0;  // 3 the last used, 2 listing the model, else 1
    for (RelayPeer *p = e->relay->peers; p; p = p->next) {
        if (!relayeligible(p, ep, s->rrelay)) {
            continue;
        }
        i32 k = p->peer==s->relaypeer ? 3 : relaylists(p, ep, model) ? 2 : 1;
        if (k>rank || (k==rank && p->seq>best->seq)) {
            best = p;
            rank = k;
        }
    }
    if (!best) {
        return 0;
    }
    s->relaypeer  = best->peer;
    s->relaybylen = (i32)best->machine.len;
    copybytes(s->relayby, best->machine.data, best->machine.len);
    return 1;
}

static void relayend(RelayCall *c, i32 err, Str why)
{
    if (!c->ended && !c->failing) {
        iz n = why.len<(iz)sizeof(c->whybuf) ? why.len : (iz)sizeof(c->whybuf);
        copybytes(c->whybuf, why.data, n);
        c->failing = 1;
        c->err     = err;
        c->why     = Str(c->whybuf, n);
    }
}

static void relayfailpeer(Engine *e, RelayPeer *p, Str why)
{
    for (RelayCall *c = e->relay->calls; c; c = c->next) {
        if (c->peer == p->peer) {
            relayend(c, HTTP_ELOST, why);
        }
    }
}

// Start relaying a request for session s (or none), to the computer its
// gate chose, or with no ep, a web search, to one that searches. Never
// fails here: a failure ends it at the next pass.
static RelayCall *relaystart(Engine *e, Endpoint *ep, HttpReq *req,
                             Session *s, b32 keep, Arena scratch)
{
    Relay *r    = e->relay;
    Str    pre  = ep ? concat(&scratch, ep->url, "/") : Str{};
    Str    path = !ep ? Str("search") :
                  startswith(req->url, pre) ? cuthead(req->url, pre.len) : Str{};
    i64    peer = !ep ? relaysearcher(e, s) : s ? s->relaypeer : 0;
    Str    sid  = s ? s->sid : Str{};
    Str    body = req->body;
    if (!peer && ep) {
        for (RelayPeer *p = r->peers; p; p = p->next) {
            if (relayeligible(p, ep, {}) && (!peer || p->seq>relaypeerbynum(e, peer)->seq)) {
                peer = p->peer;
            }
        }
    }

    // A delta against the last body sent there, if smaller, then
    // deflated, if that is smaller
    Str        data = body;
    u64        base = 0;
    RelayBase *b    = keep && sid.len ? relaybase(e, sid) : 0;
    if (b && b->peer==peer && body.len<=relaybodymax()) {
        Arena work = carve(&scratch, 2*b->body.len + 4096);
        Str   ops  = deltamake(&scratch, work, b->body, body);
        if (ops.len < body.len) {
            data = ops;
            base = b->hash;
        }
    }
    iz size = data.len;
    iz zlen = 0;
    if (data.len>=256 && data.len<=relaybodymax()) {
        iz  cap = data.len*9/8 + 16;
        u8 *z   = alloc<u8>(&scratch, cap);
        iz  n   = zdeflate(scratch, z, cap, data.data, data.len);
        if (n < data.len) {
            data = Str(z, n);
            zlen = n;
        }
    }

    Region *mem = os_reserve(e->os, data.len + req->url.len + sid.len + (1<<16));
    if (!mem) {
        return 0;  // out of memory: ehttp fails
    }
    Arena      a = newarena(mem);
    RelayCall *c = alloc<RelayCall>(&a);
    c->mem       = mem;
    c->id        = req->id;
    c->peer      = peer;
    c->sid       = clone(&a, sid);
    c->endpoint  = ep ? clone(&a, ep->relay) : Str{};
    c->path      = clone(&a, path);
    c->method    = clone(&a, req->method);
    c->accept    = clone(&a, cut(cut(cuthead(req->headers,
                                 find(req->headers, "Accept: ")+8), '\r').head, '\n').head);
    c->model     = s && ep ? clone(&a, reqmc(s)->name) : Str{};
    c->readms    = req->readms;
    c->anthropic = ep && ep->kind==EP_ANTHROPIC;
    c->keep      = keep && sid.len;
    c->base      = base;
    c->len       = body.len;
    c->size      = size;
    c->zlen      = zlen;
    c->data      = clone(&a, data);
    c->parts     = (i32)((data.len + r->part - 1) / r->part);
    c->parts     = c->parts ? c->parts : 1;
    c->room      = 1;
    if (c->keep) {
        relaykeep(e, sid, peer, body);
    }

    RelayPeer *p = relaypeerbynum(e, peer);
    if (!p || p->away || p->gone || !path.len || body.len>relaybodymax()) {
        relayend(c, HTTP_ELOST, !path.len ? Str("not a relayed URL") :
                 body.len>relaybodymax() ? Str("the request is too large to relay") :
                 !ep ? Str("no connected computer runs web searches now") :
                 Str("no connected computer relays it"));
    }
    RelayCall **tail = &r->calls;
    for (; *tail; tail = &(*tail)->next) {}
    *tail = c;
    return c;
}

static void relaycancel(Engine *, RelayCall *c)
{
    c->tell = c->sent>0 && !c->ended && !c->failing;
    relayend(c, HTTP_ECANCEL, "cancelled");
}

static void relayfree(Engine *e, RelayCall *c)
{
    relaycancel(e, c);
    c->freed = 1;
}

static RelayCall *relaycallbyid(Engine *e, u64 id)
{
    for (RelayCall *c = e->relay->calls; id && c; c = c->next) {
        if (c->id == id) {
            return c;
        }
    }
    return 0;
}

static void onrecord(Engine *, Arena, Record);

// Deliver one of a call's records as the platform's would arrive.
static void relaydeliver(Engine *e, Arena scratch, RelayCall *c, u32 type,
                         i32 head, Str data)
{
    Record r = {};
    r.type = type;
    r.id   = c->id;
    r.ok   = 1;
    if (type == HTTP_BODY) {
        r.data = data;
    } else {
        u8 *p = alloc<u8>(&scratch, 4 + data.len);
        copybytes(p, &head, 4);
        copybytes(p+4, data.data, data.len);
        r.data = Str(p, 4 + data.len);
    }
    if (type == HTTP_DONE) {
        c->ended = 1;
    }
    onrecord(e, scratch, r);
}

static i32 relayerrcode(Str err)
{
    return err=="ok"      ? HTTP_OK       :
           err=="cancel"  ? HTTP_ECANCEL  :
           err=="connect" ? HTTP_ECONNECT :
           err=="toobig"  ? HTTP_ETOOBIG  :
           err=="trust"   ? HTTP_ETRUST   :
           err=="nobase"  ? HTTP_ENOBASE  :
           err=="io"      ? HTTP_EIO      : HTTP_ELOST;
}

static Str relayerrname(i32 err)
{
    switch (err) {
    case HTTP_OK:       return "ok";
    case HTTP_ECANCEL:  return "cancel";
    case HTTP_ECONNECT: return "connect";
    case HTTP_ETOOBIG:  return "toobig";
    case HTTP_ETRUST:   return "trust";
    case HTTP_ENOBASE:  return "nobase";
    case HTTP_EIO:      return "io";
    }
    return "lost";
}

// relay.data {rid, peer, head|body|done|queued|sent}: part of the answer
// to a call, from the computer it went to, which the host stamped.
static void relaydata(Engine *e, Arena scratch, Json *j)
{
    RelayCall *c = relaycallbyid(e, (u64)jsonint(jsonget(j, "rid"), 0));
    if (!c || c->ended || c->failing || c->peer!=jsonint(jsonget(j, "peer"), -1)) {
        return;
    }
    Session *s = 0;
    for (Session *t = e->sessions; t; t = t->next) {
        s = t->httpid==c->id ? t : s;
    }
    Json *x = 0;
    if ((x = jsonget(j, "queued"))->type == JSON_OBJECT) {
        if (s && s->state==S_HEAD) {
            // Waiting there, no watchdog runs: none would end the wait
            s->relayq    = 1;
            s->relayqpos = (i32)jsonint(jsonget(x, "pos"), 0);
            s->relayqlen = (i32)jsonint(jsonget(x, "len"), 0);
            disarm(s, T_FIRSTBYTE);
            live(s);
        }
    } else if (jsonget(j, "sent")->type != JSON_MISSING) {
        if (s && s->state==S_HEAD && s->relayq) {
            s->relayq = 0;
            arm(s, T_FIRSTBYTE, reqep(s)->firstbyte);
            live(s);
        }
    } else if ((x = jsonget(j, "head"))->type == JSON_OBJECT) {
        i32 status  = (i32)jsonint(jsonget(x, "status"), 0);
        Str headers = jsonstr(jsonget(x, "headers"));
        if (s) {
            s->relayq = 0;
        }
        relaydeliver(e, scratch, c, HTTP_HEAD, status, headers);
    } else if ((x = jsonget(j, "body"))->type == JSON_STRING) {
        Unbase64 d = unbase64(&scratch, jsonstr(x));
        if (!d.ok) {
            relayend(c, HTTP_ELOST, "the relayed response was unsound");
            return;
        }
        relaydeliver(e, scratch, c, HTTP_BODY, 0, d.data);
    } else if ((x = jsonget(j, "done"))->type == JSON_OBJECT) {
        i32 err  = relayerrcode(jsonstr(jsonget(x, "err")));
        Str text = jsonstr(jsonget(x, "text"));
        if (err==HTTP_ENOBASE && c->sid.len) {
            relaydropbase(e, c->sid);  // the next goes whole
        }
        relaydeliver(e, scratch, c, HTTP_DONE, err, text);
    }
}

// relay.more {rid}: the host has room for the call's next part.
static void relaymore(Engine *e, Json *j)
{
    RelayCall *c = relaycallbyid(e, (u64)jsonint(jsonget(j, "rid"), 0));
    if (c && c->room<c->parts) {
        c->room++;
    }
}

// relay.peer {peer, away|back|gone}: the host's news of a client. Calls
// that went to one that is away or gone are lost at once.
static void relaypeernews(Engine *e, Arena scratch, Json *j)
{
    RelayPeer *p     = relaypeerbynum(e, jsonint(jsonget(j, "peer"), 0));
    Str        state = jsonstr(jsonget(j, "state"));
    if (!p) {
        return;
    } else if (state == "back") {
        p->away = 0;
        relaywake(e, scratch);
    } else if (state=="away" || state=="gone") {
        p->away  = 1;
        p->gone |= state == "gone";
        Str why = concat(&scratch, p->machine, state=="gone" ?
                         Str(", which relayed it, disconnected") :
                         Str(", which relayed it, lost its connection"));
        relayfailpeer(e, p, why);
    }
}


// The serving side

static RelayServe *relayservebyadm(Engine *e, u64 id)
{
    for (RelayServe *v = e->relay->serves; id && v; v = v->next) {
        if (v->admid == id) {
            return v;
        }
    }
    return 0;
}

static void relayfinish(RelayServe *v, Str err, Str text)
{
    if (!v->done) {
        v->done = 1;
        v->err  = clone(&v->arena, err);
        v->text = clone(&v->arena, text);
    }
}

// Give up its place at the endpoint.
static void relayrelease(Engine *e, RelayServe *v)
{
    if (v->slot == SLOT_NONE) {
        return;
    }
    if (v->slot == SLOT_HELD) {
        admdone(v->point, v->slotmodel, os_ticks(e->os), 0,
                scaled(e, ADM_CALM));
    } else {
        admleave(v->point, v->admid);
    }
    v->slot = SLOT_NONE;
    pump(e, v->point);
}

// Admitted at its endpoint (pump): it goes at the next pass.
static void relayadmitted(Engine *e, u64 id)
{
    RelayServe *v = relayservebyadm(e, id);
    assert(v && v->slot==SLOT_WAIT);
    v->slot     = SLOT_HELD;
    v->admepoch = admlane(v->point, 0, v->slotmodel)->epoch;
    v->go       = 1;
    v->queued   = 0;
    v->sent     = v->qlen > 0;  // it was told it waited
}

// Calls waiting at p hear of their places as they change.
static void relayplaces(Engine *e, AdmPoint *p)
{
    for (RelayServe *v = e->relay->serves; v; v = v->next) {
        if (v->point==p && v->slot==SLOT_WAIT &&
                (admpos(p, v->admid)!=v->qpos || admlen(p)!=v->qlen)) {
            v->qpos   = admpos(p, v->admid);
            v->qlen   = admlen(p);
            v->queued = 1;
        }
    }
}

// Its turn at its endpoint, as a session's own request waits for it
// (admit), for the model the call names.
static void relayadmit(Engine *e, Arena scratch, RelayServe *v)
{
    Config   *cfg = e->cfg;
    AdmPoint *p   = admpoint(&e->adm, &e->perm, v->ep.url);
    i64      *k   = upsert(&e->kinds, kindkey(&scratch, v->ep.url, v->model), (Arena *)0);
    b32       ll  = k && (*k&1);
    i32       sl  = k ? (i32)(*k>>1) : 0;
    p->maxreq    = confint(cfg, "endpoint", v->ep.name, "max_concurrent");
    p->cfgmodels = confint(cfg, "endpoint", v->ep.name, "max_models");
    if (!p->cfgmodels && ll && p->probe==PROBE_NONE) {
        probe(e, p, &v->ep, scratch);
    }
    pointlimits(p);
    i32 cap = confint(cfg, "model", v->model, "max_concurrent");
    cap = cap ? cap : sl ? sl : p->slots ? p->slots : 4;
    v->admid     = ++e->nextid;
    v->point     = p;
    v->slot      = SLOT_WAIT;
    v->slotmodel = v->model;
    admenqueue(p, &e->perm, v->admid, v->model, cap, ll || p->cfgmodels,
               os_ticks(e->os));
    pump(e, p);
}

// Its request, to its endpoint, with this engine's key.
static void relaysend(Engine *e, Arena scratch, RelayServe *v)
{
    v->go = 0;
    Str path = v->path;
    Str url  = startswith(path, "v1/") ? apiurl(&scratch, v->ep.url, cuthead(path, 3))
                                       : rooturl(&scratch, v->ep.url, path);
    HttpReq r = {};
    r.method    = v->method;
    r.url       = url;
    r.headers   = authheaders(&scratch, &v->ep, v->accept);
    r.body      = v->body;
    r.connectms = v->ep.connectms;
    r.readms    = v->readms;
    r.maxbytes  = RESPONSE_MAX;
    r.mbox      = e->in;
    r.id        = v->httpid = ++e->nextid;
    v->http     = os_http(e->os, &r, scratch);
    if (!v->http) {
        v->httpid = 0;
        relayrelease(e, v);
        relayfinish(v, "connect", os_lasterr(e->os, &scratch));
    }
}

// A served call's path, as an endpoint's: only the API's calls that
// sessions make, so that the key serves nothing else.
static b32 relaypath(Str path, Str method)
{
    Str p = cut(path, '?').head;
    if (method=="POST") {
        return p=="v1/messages" || p=="v1/chat/completions" ||
               p=="v1/chat/completions/control";
    } else if (method=="GET") {
        return p=="v1/models" || p=="props";
    }
    return 0;
}

// Its body, once all parts came: inflated, then the delta applied to the
// base kept for its origin's session. Null if that base is not the one
// the delta needs, or anything is unsound.
static Str relaybody(Engine *e, RelayServe *v, Str *why)
{
    Str data = finish(&v->data);
    if (v->zlen) {
        iz    n     = wuffsinflatestate();
        u8   *mem   = alloc<u8>(&v->arena, n+15);
        void *state = (void *)(((uz)mem + 15) & ~(uz)15);
        u8   *out   = alloc<u8>(&v->arena, v->size+1);
        if (wuffsinflate(state, out, v->size, data.data, data.len)) {
            *why = "lost";
            return {};
        }
        data = Str(out, v->size);
    } else if (data.len != v->size) {
        *why = "lost";
        return {};
    }
    Str key = concat(&v->arena, concat(&v->arena, v->origin, " "), v->session);
    if (v->base) {
        RelayBase *b = relaybase(e, key);
        Str body = b && b->hash==v->base ? deltaapply(&v->arena, b->body, data, v->len)
                                         : Str{};
        if (!body.data) {
            relaydropbase(e, key);
            *why = "nobase";
            return {};
        }
        data = body;
    } else if (data.len != v->len) {
        *why = "lost";
        return {};
    }
    if (v->keep && v->session.len) {
        relaykeep(e, key, 0, data);
    }
    return data;
}

static u64 relayhex(Str s)
{
    u64 v = 0;
    for (iz i = 0; i<s.len && i<16; i++) {
        i32 d = hexval(s[i]);
        v = v<<4 | (u64)(d<0 ? 0 : d);
    }
    return v;
}

static RelayServe *relayservebycid(Engine *e, Str origin, i64 cid)
{
    for (RelayServe *v = e->relay->serves; v; v = v->next) {
        if (v->cid==cid && v->origin==origin) {
            return v;
        }
    }
    return 0;
}

static void relayservecancel(Engine *e, RelayServe *v)
{
    if (v->web) {
        webfree(e, v->web);
        v->web = 0;
        relayfinish(v, "cancel", "cancelled");
    } else if (v->http) {
        os_httpcancel(e->os, v->http);  // its HTTP_DONE ends it
    } else {
        relayrelease(e, v);
        relayfinish(v, "cancel", "cancelled");
    }
}

// The start of a served call's relay.data: a record for the client that
// passed the call on, or, for a hostlink's (far), a command to its host.
static EvOut relaydatabegin(Engine *e, Buf *b, HostLink *far, i64 cid, Str origin)
{
    if (far) {
        EvOut w = cmdbegin(b, CMD_RELAYDATA, 0);
        putintkey(&w.json, "rid", cid);
        return w;
    }
    EvOut w = evbegin(b, {}, 0, os_time(e->os), EV_RELAYDATA);
    putintkey(&w.json, "cid", cid);
    putstrkey(&w.json, "origin", origin);
    return w;
}

static b32 farrelay(HostLink *, Str);

// A thin engine's web search, run as one of a session's is, but with no
// session: its records come by relayrecord, its deadline by relaytick.
static void relaysearch(Engine *e, Arena scratch, RelayServe *v)
{
    JsonDoc d     = jsonparse(&scratch, v->body);
    Str     query = jsonstr(jsonget(d.root, "query"));
    Region *mem   = query.len ? os_reserve(e->os, WEB_REGION) : 0;
    if (!mem) {
        relayfinish(v, "connect", query.len ? Str("out of memory") : Str("no query"));
        return;
    }
    Arena   a = newarena(mem);
    WebRun *w = alloc<WebRun>(&a);
    w->region  = mem;
    w->arena   = a;
    w->v       = v;
    w->tool    = TOOL_WEBSEARCH;
    w->service = websearchservice(e);
    w->query   = clone(&w->arena, query);
    Str why = {};
    if (w->service == WEBS_ANTHROPIC) {
        why = webanthropicep(e, &w->arena, &w->ep, &w->model);
    }
    why = why.len ? why : websend(e, w, scratch);
    if (why.len) {
        relayfinish(v, "connect", why);  // before its arena goes
        webfree(e, w);
        return;
    }
    v->web   = w;
    v->webby = os_ticks(e->os) + (w->service==WEBS_ANTHROPIC ? 2*e->minute : e->minute/2);
}

// A relayed search ended: its result, as the body of a response.
static void relaysearched(Engine *e, Arena scratch, RelayServe *v, Str status,
                          Str text, i32 searches)
{
    Buf     b(&scratch, 256 + 2*text.len);
    JsonOut w = {&b};
    putobject(&w);
    putstrkey(&w, "status", status);
    putstrkey(&w, "text", text);
    putintkey(&w, "searches", searches);
    putend(&w);
    Str body = finish(&b);
    if (body.len > RELAY_HOLD) {
        body = "{\"status\":\"error\",\"text\":\"Not done: the result was too large.\"}";
    }
    webfree(e, v->web);  // after the text, which may be its
    v->web     = 0;
    v->status  = 200;
    v->headers = "Content-Type: application/json\r\n";
    v->head    = 1;
    copybytes(v->hold, body.data, body.len);
    v->held    = body.len;
    relayfinish(v, "ok", {});
}

// Part of a thin engine's call, which its client passed on (relay.call)
// or a hostlink's host sent (far), or a cancel of one, or of every call
// of the origin without a cid.
static void relayserve(Engine *e, Arena scratch, Json *j, i64 cid, Str origin,
                       HostLink *far)
{
    Relay *r = e->relay;
    if (jsonflag(jsonget(j, "cancel"), 0)) {
        for (RelayServe *v = r->serves; v; v = v->next) {
            b32 mine = cid ? v->cid==cid && (!origin.len || v->origin==origin)
                           : v->origin==origin;
            if (mine && !v->done) {
                relayservecancel(e, v);
            }
        }
        return;
    } else if (cid<=0 || origin.len>256) {
        return;
    }

    i32         part = (i32)jsonint(jsonget(j, "part"), -1);
    RelayServe *v    = relayservebycid(e, origin, cid);
    if (!v && part==0) {
        iz   max   = relaybodymax();
        i64  len   = jsonint(jsonget(j, "len"), -1);
        i64  size  = jsonint(jsonget(j, "size"), -1);
        i64  zlen  = jsonint(jsonget(j, "zlen"), 0);
        i64  parts = jsonint(jsonget(j, "parts"), 0);
        i64  total = zlen ? zlen : size;
        b32  sound = len>=0 && len<=max && size>=0 && size<=max &&
                     zlen>=0 && zlen<=size+size/8+64 &&
                     parts>=1 && parts<=total/RELAY_PART+1;
        Region *mem = sound ? os_reserve(e->os, (iz)(total + size + len) + (1<<20) + RELAY_HOLD) : 0;
        if (!mem) {
            // Unsound, or no memory for it: answered at once, from
            // scratch (a call never made)
            Buf   b(&scratch, 256 + origin.len);
            EvOut w = relaydatabegin(e, &b, far, cid, origin);
            putkey(&w.json, "done");
            putobject(&w.json);
            putstrkey(&w.json, "err", "lost");
            putstrkey(&w.json, "text", sound ? Str("out of memory") : Str("an unsound relayed call"));
            putend(&w.json);
            if (far) {
                farrelay(far, evend(&w));  // or never: the call ends there
            } else {
                emit(e, evend(&w));
            }
            return;
        }
        Arena a = newarena(mem);
        v = alloc<RelayServe>(&a);
        v->arena   = a;
        v->mem     = mem;
        v->cid     = cid;
        v->far     = far;
        v->origin  = clone(&v->arena, origin);
        v->session = clone(&v->arena, jsonstr(jsonget(j, "session")));
        v->path    = clone(&v->arena, jsonstr(jsonget(j, "path")));
        v->method  = clone(&v->arena, jsonstr(jsonget(j, "method")));
        v->accept  = clone(&v->arena, jsonstr(jsonget(j, "accept")));
        v->model   = clone(&v->arena, jsonstr(jsonget(j, "model")));
        i64 readms = jsonint(jsonget(j, "readms"), 0);
        v->readms  = (i32)(readms>0 && readms<3600000 ? readms : 0);
        v->keep    = jsonflag(jsonget(j, "keep"), 0);
        v->base    = relayhex(jsonstr(jsonget(j, "base")));
        v->len     = (iz)len;
        v->size    = (iz)size;
        v->zlen    = (iz)zlen;
        v->parts   = (i32)parts;
        v->data    = Buf(&v->arena, (iz)total + 1);
        v->hold    = alloc<u8>(&v->arena, RELAY_HOLD);
        v->errbody = Buf(&v->arena, 1<<12);
        v->next    = r->serves;
        r->serves  = v;

        // Only its own endpoints, of the kind asked, at their API's paths,
        // or a web search, with its own service
        Str name = jsonstr(jsonget(j, "endpoint"));
        b32 an   = jsonflag(jsonget(j, "anthropic"), 0);
        b32 ok   = relayname(name) && endpointconf(e, &v->arena, name, &v->ep) &&
                   v->ep.name==name && !v->ep.relay.len;
        i32 svc  = websearchservice(e);
        v->search = v->path=="search";
        if (v->search) {
            Str why = svc==WEBS_NONE || svc==WEBS_RELAY ? Str("web search is off here")
                                                        : webunready(e, &scratch);
            if (v->method!="POST" || why.len) {
                relayfinish(v, "connect", why.len ? why : Str("not a search"));
            }
        } else if (!ok) {
            relayfinish(v, "connect", concat(&scratch, "no endpoint here is named ", name));
        } else if ((v->ep.kind==EP_ANTHROPIC) != an) {
            relayfinish(v, "connect", concat(&scratch, name, " is of another kind here"));
        } else if (!relaypath(v->path, v->method)) {
            relayfinish(v, "connect", "not a call relayed calls may make");
        }
    }
    if (!v || v->done || part!=v->got) {
        return;  // a late part of one that failed, or out of order
    }

    Unbase64 d = unbase64(&scratch, jsonstr(jsonget(j, "data")));
    if (!d.ok || d.data.len>RELAY_PART || v->data.len+d.data.len>v->data.cap-1) {
        relayfinish(v, "lost", "an unsound part of a relayed call");
        return;
    }
    print(&v->data, d.data);
    if (++v->got < v->parts) {
        return;
    }
    Str why = {};
    v->body = relaybody(e, v, &why);
    if (!v->body.data) {
        relayfinish(v, why, why=="nobase" ? Str("no base for the delta") :
                                            Str("an unsound relayed call"));
        return;
    }
    if (v->search) {
        relaysearch(e, scratch, v);
        return;
    }
    Str p = cut(v->path, '?').head;
    v->admits = p=="v1/messages" || p=="v1/chat/completions";
    if (v->admits) {
        relayadmit(e, scratch, v);
    } else {
        v->go = 1;
    }
}

// relay.call {cid, origin, ...}: part of a thin engine's call, which
// its client passed on.
static void relaycall(Engine *e, Arena scratch, Json *j)
{
    i64 cid    = jsonint(jsonget(j, "cid"), 0);
    Str origin = jsonstr(jsonget(j, "origin"));
    relayserve(e, scratch, j, cid, origin, 0);
}

// A served call's end is gone: its calls are cancelled, and nothing more
// of them is sent.
static void relaydropfar(Engine *e, HostLink *far)
{
    for (RelayServe *v = e->relay->serves; v; v = v->next) {
        if (v->far == far) {
            v->dropped = 1;
            if (!v->done) {
                relayservecancel(e, v);
            }
        }
    }
}

// A record of a served call's request. Returns 0 if it is not one.
static b32 relayrecord(Engine *e, Arena scratch, Record r)
{
    RelayServe *v = 0;
    for (RelayServe *t = e->relay->serves; r.id && !v && t; t = t->next) {
        v = t->httpid==r.id || (t->web && t->web->hid==r.id) ? t : 0;
    }
    if (!v) {
        return 0;
    } else if (v->web) {
        webrecord(e, v->web, scratch, r);
        return 1;
    }
    switch (r.type) {
    case HTTP_HEAD:
        v->status  = get32(r.data);
        v->headers = clone(&v->arena, cuthead(r.data, 4));
        v->head    = 1;
        v->sent    = 0;
        break;
    case HTTP_BODY:
        if (v->status>=400 && v->errbody.len+r.data.len<=v->errbody.cap) {
            print(&v->errbody, r.data);
        }
        if (v->held+r.data.len > RELAY_HOLD) {
            // Its client takes it too slowly: let it go, as lost
            os_httpcancel(e->os, v->http);
            relayfinish(v, "lost", "the relayed response was taken too slowly");
            break;
        }
        copybytes(v->hold+v->held, r.data.data, r.data.len);
        v->held += r.data.len;
        break;
    case HTTP_DONE: {
        os_httpfree(e->os, v->http);
        v->http   = 0;
        v->httpid = 0;
        if (v->slot==SLOT_HELD && contains(finish(&v->errbody), "Context size has been exceeded")) {
            // As fail does for a session's: llama.cpp's shared pool is full
            admshrink(v->point, v->slotmodel, v->admepoch, os_ticks(e->os),
                      scaled(e, ADM_CALM));
        }
        relayrelease(e, v);
        if (v->status==200 && v->method=="GET" && cut(v->path, '?').head=="v1/models") {
            Arena a = scratch;
            notekinds(e, a, v->ep.url, Str(v->hold, v->held));
        }
        relayfinish(v, relayerrname(get32(r.data)), cuthead(r.data, 4));
    } break;
    }
    return 1;
}


// Records out, held back while the host's inbox is too full for them

// Post a line only if it leaves the mailbox half free (mailroom), or a
// served call's for a hostlink, only if its link has room (farrelay).
static b32 relayemit(Engine *e, Str line, RelayServe *v = 0)
{
    if (v && v->far) {
        b32 ok = farrelay(v->far, line);
        e->relay->farheld |= !ok;
        return ok;
    } else if (!mailroom(e->out, line.len)) {
        e->relay->held = 1;
        return 0;
    }
    emit(e, line);
    return 1;
}

static Str relaypart(Engine *e, Arena *a, RelayCall *c)
{
    iz    part = e->relay->part;
    iz    off  = (iz)c->sent * part;
    iz    n    = c->data.len-off<part ? c->data.len-off : part;
    Str   data = base64(a, Str(c->data.data+off, n));
    Buf   b(a, 1024 + data.len + c->sid.len + c->path.len + c->model.len);
    EvOut w = evbegin(&b, {}, 0, os_time(e->os), EV_RELAY);
    putintkey(&w.json, "rid", (i64)c->id);
    putintkey(&w.json, "peer", c->peer);
    if (!c->sent) {
        putstrkey(&w.json, "session", c->sid);
        putstrkey(&w.json, "endpoint", c->endpoint);
        putintkey(&w.json, "anthropic", c->anthropic);
        putstrkey(&w.json, "path", c->path);
        putstrkey(&w.json, "method", c->method);
        putstrkey(&w.json, "accept", c->accept);
        putstrkey(&w.json, "model", c->model);
        putintkey(&w.json, "readms", c->readms);
        if (c->keep) {
            putintkey(&w.json, "keep", 1);
        }
        if (c->base) {
            Buf h(a, 16);
            printhex(&h, c->base, 16);
            putstrkey(&w.json, "base", finish(&h));
        }
        putintkey(&w.json, "len", c->len);
        putintkey(&w.json, "size", c->size);
        if (c->zlen) {
            putintkey(&w.json, "zlen", c->zlen);
        }
    }
    putintkey(&w.json, "part", c->sent);
    putintkey(&w.json, "parts", c->parts);
    putstrkey(&w.json, "data", data);
    return evend(&w);
}

static Str relayout(Engine *e, Arena *a, RelayServe *v, Str key)
{
    Buf   b(a, 1024 + v->origin.len + v->headers.len + v->text.len);
    EvOut w = relaydatabegin(e, &b, v->far, v->cid, v->origin);
    putkey(&w.json, key);
    if (key == "queued") {
        putobject(&w.json);
        putintkey(&w.json, "pos", v->qpos);
        putintkey(&w.json, "len", v->qlen);
        putend(&w.json);
    } else if (key == "sent") {
        putint(&w.json, 1);
    } else if (key == "head") {
        putobject(&w.json);
        putintkey(&w.json, "status", v->status);
        putstrkey(&w.json, "headers", v->headers);
        putend(&w.json);
    } else if (key == "body") {
        iz n = v->held<RELAY_PART ? v->held : (iz)RELAY_PART;
        putstr(&w.json, base64(a, Str(v->hold, n)));
    } else {
        putobject(&w.json);
        putstrkey(&w.json, "err", v->err);
        putstrkey(&w.json, "text", v->text);
        putend(&w.json);
    }
    return evend(&w);
}

// What a served call has to send back, in order, while there is room.
static b32 relayserveout(Engine *e, Arena scratch, RelayServe *v)
{
    if (v->dropped) {
        v->queued   = v->sent = v->head = 0;
        v->held     = 0;
        v->finished = v->done;
        return 1;
    }
    if (v->queued) {
        if (!relayemit(e, relayout(e, &scratch, v, "queued"), v)) return 0;
        v->queued = 0;
    }
    if (v->sent) {
        if (!relayemit(e, relayout(e, &scratch, v, "sent"), v)) return 0;
        v->sent = 0;
    }
    if (v->head) {
        if (!relayemit(e, relayout(e, &scratch, v, "head"), v)) return 0;
        v->head = 0;
    }
    while (v->held) {
        Arena a = scratch;
        if (!relayemit(e, relayout(e, &a, v, "body"), v)) return 0;
        iz n = v->held<RELAY_PART ? v->held : (iz)RELAY_PART;
        v->held -= n;
        copybytes(v->hold, v->hold+n, v->held);  // memmove
    }
    if (v->done && !v->finished) {
        if (!relayemit(e, relayout(e, &scratch, v, "done"), v)) return 0;
        v->finished = 1;
    }
    return 1;
}

// Each pass: calls fail as they must, parts go as there is room, served
// calls go once admitted and answer, and what has ended goes.
static void relaytick(Engine *e, Arena scratch)
{
    Relay *r = e->relay;
    r->held    = 0;
    r->farheld = 0;
    for (RelayCall *c = r->calls; c; c = c->next) {
        Arena a = scratch;
        if (c->failing && !c->ended) {
            c->failing = 0;
            relaydeliver(e, a, c, HTTP_DONE, c->err, c->why);
        }
        if (c->tell && !r->held) {
            Buf   b(&a, 256);
            EvOut w = evbegin(&b, {}, 0, os_time(e->os), EV_RELAY);
            putintkey(&w.json, "rid", (i64)c->id);
            putintkey(&w.json, "peer", c->peer);
            putintkey(&w.json, "cancel", 1);
            c->tell = !relayemit(e, evend(&w));
        }
        while (!c->ended && !r->held && c->sent<c->room && c->sent<c->parts) {
            Arena t = a;
            if (relayemit(e, relaypart(e, &t, c))) {
                c->sent++;
            }
        }
    }
    i64 now = os_ticks(e->os);
    for (RelayServe *v = r->serves; v; v = v->next) {
        if (v->go) {
            relaysend(e, scratch, v);
        }
        if (v->web && now>=v->webby) {
            Str text = concat(&scratch, concat(&scratch, "Timed out: ",
                              webservices[v->web->service]), " did not answer in time.");
            relaysearched(e, scratch, v, "timeout", text, 1);
        }
        if (!r->held || v->far) {
            relayserveout(e, scratch, v);
        }
    }

    for (RelayCall **p = &r->calls; *p;) {
        RelayCall *c = *p;
        if (c->ended && c->freed && !c->tell) {
            *p = c->next;
            os_release(e->os, c->mem);
        } else {
            p = &c->next;
        }
    }
    for (RelayServe **p = &r->serves; *p;) {
        RelayServe *v = *p;
        if (v->finished && !v->http && !v->web && v->slot==SLOT_NONE) {
            *p = v->next;
            os_release(e->os, v->mem);
        } else {
            p = &v->next;
        }
    }
}

// When relaytick has work: at once, or soon while records are held back.
static i64 relaynext(Engine *e, i64 now)
{
    Relay *r = e->relay;
    b32    work = 0;
    for (RelayCall *c = r->calls; !work && c; c = c->next) {
        work = (c->failing && !c->ended) || c->tell ||
               (!c->ended && c->sent<c->room && c->sent<c->parts) ||
               (c->ended && c->freed);
    }
    i64 next = I64MAX;
    for (RelayServe *v = r->serves; v; v = v->next) {
        work |= v->go || v->queued || v->sent || v->head || v->held ||
                (v->done && !v->finished) || (v->finished && !v->http && !v->web);
        next  = v->web && v->webby<next ? v->webby : next;
    }
    if (!work) {
        return next;
    }
    return r->held || r->farheld ? now + (i64)RELAY_RETRY*MS : now;
}


// The offer: this engine's models, as a thin engine may relay them

// Whether an offer has an endpoint: one with a URL of its own.
static b32 relayoffers(Engine *e, Arena scratch, Str name)
{
    Endpoint ep = {};
    return relayname(name) && endpointconf(e, &scratch, name, &ep) && !ep.relay.len;
}

// A model's settings for an offer, into w, flattened through like.
static void relaymodelput(Engine *e, JsonOut *w, Arena scratch, Str name)
{
    Config *cfg = e->cfg;
    putobject(w);
    putstrkey(w, "name", name);
    putkey(w, "settings");
    putobject(w);
    // Its id, which its like gives by default, since like goes
    Str id = cfgmodelid(cfg, name);
    if (!(id == name)) {
        putstrkey(w, "id", id);
    }
    for (iz i = 0; i < countof(cfgschema); i++) {
        CfgKey *k = &cfgschema[i];
        if (!(k->kind=="model") || cfgfamily(k) || !relaymodelkey(k->key) ||
                k->key=="id") {
            continue;
        }
        IniEntry *x = cfgfind(cfg, "model", name, k->key).e;
        if (x && x->line->valbeg<x->line->valend) {
            IniLine *l = x->line;
            putstrkey(w, k->key, Str(l->raw.data+l->valbeg, l->valend-l->valbeg));
        }
    }
    // Template arguments, merged along the chain, as written
    Str chain[CFG_LIKEMAX];
    i32 len = cfglikechain(e->ini, name, chain);
    Slice<IniEntry *> args = {};
    for (i32 i = len-1; i >= 0; i--) {
        IniSection *t = inisection(e->ini, "model", chain[i]);
        for (IniEntry *x = t ? t->keys : 0; x; x = x->next) {
            if (!cfgtemplatename(x->key).len) {
                continue;
            }
            iz k = 0;
            for (; k<args.len && !(args[k]->key==x->key); k++) {}
            if (k == args.len) {
                args = push(&scratch, args, x);
            } else {
                args[k] = x;
            }
        }
    }
    for (iz i = 0; i < args.len; i++) {
        IniLine *l = args[i]->line;
        putstrkey(w, args[i]->key, Str(l->raw.data+l->valbeg, l->valend-l->valbeg));
    }
    putend(w);
    putend(w);
}

// An offer, for relay.offer, into w: each endpoint that has a URL of its
// own, its settings that a thin engine needs, and the models its last
// listing gave, but never its URL or key; each [model] section flattened
// through like, and for each offered endpoint whose chain differs there,
// its ENDPOINT/MODEL too, as like no longer joins the two; the defaults;
// whether it searches; and own, a hostlink's token, for the sessions it
// made alone.
static void relayofferput(Engine *e, JsonOut *w, Arena scratch, Str own)
{
    Config *cfg = e->cfg;
    putobject(w);
    putstrkey(w, "machine", cut(os_hostname(e->os, &scratch), '.').head);
    if (own.len) {
        putstrkey(w, "own", own);
    }
    Str dep = trim(cfgget(cfg, "", "", "endpoint", 0));
    Str dmo = trim(cfgget(cfg, "", "", "model", 0));
    Str web = trim(cfgget(cfg, "web", "", "search", 0));
    putintkey(w, "search", web.len && !(web=="none"));

    b32 named = 0;
    putkey(w, "endpoints");
    putarray(w);
    for (IniSection *s = inifirst(e->ini, "endpoint"); s; s = ininext(s)) {
        Arena    a  = scratch;
        Endpoint ep = {};
        if (!relayoffers(e, a, s->name)) {
            continue;  // relays do not chain
        }
        endpointconf(e, &a, s->name, &ep);
        Url u     = parseurl(ep.url);
        b32 local = u.ok && localhost(u.host);
        named |= s->name == dep;
        putobject(w);
        putstrkey(w, "name", s->name);
        putstrkey(w, "kind", ep.kind==EP_ANTHROPIC ? Str("anthropic") : Str("openai"));
        for (iz i = 0; i < countof(relayepkeys); i++) {
            putstrkey(w, relayepkeys[i],
                      trim(cfgget(cfg, "endpoint", s->name, relayepkeys[i], local)));
        }
        putkey(w, "models");
        putarray(w);
        Str *list = upsert(&e->listings, ep.url, (Arena *)0);
        for (Str rest = list ? *list : Str{}; rest.len;) {
            Cut c = cut(rest, '\n');
            if (c.head.len) {
                putstr(w, c.head);
            }
            rest = c.tail;
        }
        putend(w);
        putend(w);
    }
    putend(w);
    if (named) {
        putstrkey(w, "endpoint", dep);
    }
    if (dmo.len) {
        putstrkey(w, "model", dmo);
    }

    putkey(w, "models");
    putarray(w);
    for (IniSection *s = inifirst(e->ini, "model"); s; s = ininext(s)) {
        // Each model once, at its first section, plain or qualified
        Str bare  = cfgbaremodel(e->ini, s->name);
        b32 first = bare.len > 0;
        for (IniSection *t = inifirst(e->ini, "model"); first && t!=s; t = ininext(t)) {
            first = !(cfgbaremodel(e->ini, t->name) == bare);
        }
        if (!first) {
            continue;
        }
        if (inisection(e->ini, "model", bare)) {
            relaymodelput(e, w, scratch, bare);
        }
        for (IniSection *p = inifirst(e->ini, "endpoint"); p; p = ininext(p)) {
            if (!relayoffers(e, scratch, p->name)) {
                continue;
            }
            Str q = concat(&scratch, concat(&scratch, p->name, "/"), bare);
            Str chain[CFG_LIKEMAX];
            i32 len  = cfglikechain(e->ini, q, chain);
            b32 qual = 0;
            for (i32 i = 0; i < len; i++) {
                qual |= cfgsplitmodel(e->ini, chain[i]).ok &&
                        inisection(e->ini, "model", chain[i]);
            }
            if (qual) {
                relaymodelput(e, w, scratch, q);
            }
        }
    }
    putend(w);
    putend(w);
}

// offer: the reply's offer, for a client to pass on as relay.offer.
static void cmdoffer(Engine *e, Arena scratch, i64 id)
{
    Arena work = carve(&scratch, (iz)1<<20);
    Buf   b(&scratch, 1<<16);
    EvOut w = evbegin(&b, {}, 0, os_time(e->os), EV_REPLY);
    putintkey(&w.json, "id", id);
    putintkey(&w.json, "ok", 1);
    putkey(&w.json, "offer");
    relayofferput(e, &w.json, work, {});
    emit(e, evend(&w));
}
