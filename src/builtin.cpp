// The built-in model in the window: Gemma 4 E2B (gemma.cpp, gemmachat.cpp)
//
// An endpoint of kind builtin has no URL: the engine relays each call to
// it (relay.cpp) as it would to another computer, and the window, a
// relay peer of its own engine, answers as an OpenAI server would.
//
// Each time it reads the configuration, the window offers its builtin
// endpoints to the main engine (relay.offer), as peer BL_SELF when it
// runs the engine itself; a host stamps its own number on it, and on
// what comes back. Calls come in parts, each asked for with relay.more
// (which a host keeps to itself, asking for them on its own). A model
// listing is answered at once, on the UI thread. Chats go to the
// model's thread, which starts with the first, loads the model then,
// keeps it for the window's life, and answers one call at a time, in
// order (the engine sends one at a time anyway: max_concurrent = 1). Its
// relay.data lines come back through a mailbox, which the UI thread
// drains to the engine, holding what the engine cannot take yet.
//
// The answer's head goes only once the model is loaded, so the wait for
// it is the engine's first-byte timeout (30 minutes for a local
// endpoint), not its idle timeout. Errors that don't go away by
// themselves (the file missing, a processor that can't run it, a prompt
// too long) are 400s with the reason, so that the turn ends with it.
//
// The window going to the background hangs up (relay.peer gone): the
// engine's turns on builtin endpoints wait until a window offers again.
//
// This is free and unencumbered software released into the public domain.

enum : i64 {
    BL_SELF = (i64)1 << 40,  // its peer number, never a host's
};

enum : i32 {
    BL_CALLS  = 8,        // calls at once: one chat, and listings
    BL_MAIL   = 1 << 20,  // bytes of lines for the UI thread
    BL_PIECE  = 192 << 10,  // SSE bytes per line, well within BL_MAIL
    BL_MINRAM = 6,        // GiB
};

enum : u32 {
    BL_LINE = 1,  // a relay.data line for the engine, id its call's slot
    BL_FREE = 2,  // the model's thread is done with that slot
};

enum : i32 {
    BL_EMPTY,
    BL_PARTS,    // its parts coming, the UI thread's
    BL_QUEUED,   // for the model's thread
    BL_RUNNING,  // the model's thread's, until BL_FREE
};

static Str const blfile = "gemma-4-e2b-ad5ae441.gguf";

struct BlCall {
    Region *mem;       // its own: freed by whoever ends the call
    i64     rid;
    i64     peer;
    Str     sid;
    Str     endpoint;
    u8     *body;
    iz      len;
    iz      got;
    i64     seq;       // the order queued
    i32     state;     // under the lock
    i32     cancel;    // atomic: the engine cancelled it
    b32     dead;      // the UI thread's: its lines go nowhere
};

struct Builtin {
    Os      *os;
    Mailbox *out;      // lines from the model's thread
    Lock     lock;
    Event    ready;    // a call queued
    Event    nap;      // never signaled: the fake's pauses
    BlCall   calls[BL_CALLS];
    i64      seq;
    Str      path;     // the model's file
    b32      fake;     // canned answers (PROSPERO_TEST_BUILTIN=fake)
    b32      started;  // the model's thread

    // The UI thread's
    Mail     pend;     // taken, not all sent
    Region  *offermem;
    Str      offered;  // the offer last sent, or null for none
    b32      offering; // an offer with endpoints is in effect

    // The model's thread's
    Arena    model;    // its weights and cache
    GmPool  *pool;
    Gemma   *g;
    GmChat  *chat;
    Region  *emitmem;
};


// Lines for the engine

// A relay.data line for call c, open for its one key.
static JsonOut blbegin(Buf *b, BlCall *c)
{
    JsonOut w = {b};
    putobject(&w);
    putstrkey(&w, "cmd", "relay.data");
    putintkey(&w, "rid", c->rid);
    putintkey(&w, "peer", c->peer);
    return w;
}

static Str blhead(Arena *a, BlCall *c, i32 status, Str type)
{
    Buf h(a, 64);
    print(&h, "Content-Type: ");
    print(&h, type);
    print(&h, "\r\n");
    Str     headers = finish(&h);
    Buf     b(a, 256);
    JsonOut w = blbegin(&b, c);
    putkey(&w, "head");
    putobject(&w);
    putintkey(&w, "status", status);
    putstrkey(&w, "headers", headers);
    putend(&w);
    putend(&w);
    return finish(&b);
}

static Str blbody(Arena *a, BlCall *c, Str data)
{
    Str     enc = base64(a, data);
    Buf     b(a, 128 + enc.len);
    JsonOut w = blbegin(&b, c);
    putstrkey(&w, "body", enc);
    putend(&w);
    return finish(&b);
}

// The call's end: err is "ok", or "lost" with why.
static Str bldone(Arena *a, BlCall *c, Str err, Str why)
{
    Buf     b(a, 256 + why.len);
    JsonOut w = blbegin(&b, c);
    putkey(&w, "done");
    putobject(&w);
    putstrkey(&w, "err", err);
    putstrkey(&w, "text", why);
    putend(&w);
    putend(&w);
    return finish(&b);
}


// The model's thread

// A line from the model's thread, waiting for room.
static void blpost(Builtin *b, BlCall *c, Str line)
{
    mailpost(b->out, BL_LINE, (u64)(c - b->calls), line, 1);
}

struct BlEmit {
    Builtin *b;
    BlCall  *c;
};

// GmOut's emit: SSE bytes, in pieces that fit the mailbox.
static void blemit(void *ctx, Str sse)
{
    BlEmit *e = (BlEmit *)ctx;
    while (sse.len) {
        Arena a = newarena(e->b->emitmem);
        iz    n = sse.len<BL_PIECE ? sse.len : (iz)BL_PIECE;
        blpost(e->b, e->c, blbody(&a, e->c, Str(sse.data, n)));
        sse = cuthead(sse, n);
    }
}

// An answer of JSON, which may lie at a's end.
static void blanswer(Builtin *b, BlCall *c, Arena *a, i32 status, Str json)
{
    blpost(b, c, blhead(a, c, status, "application/json"));
    blpost(b, c, blbody(a, c, json));
    blpost(b, c, bldone(a, c, "ok", {}));
}

// Load the model if it isn't yet. Returns why it can't be, or null.
static Str blload(Builtin *b, BlCall *c, Arena scratch)
{
    Os *os = b->os;
    if (b->g) {
        return {};
    }
    i32 isa = gmisa(os_guiarmdot(os));
    if (isa == GM_SCALAR) {
        return "This computer's processor can't run the built-in model, which "
               "needs AVX2 (x86-64) or the dot product instructions (arm64).";
    }
    i64 ram = os_guiram(os);
    if (ram && ram<((i64)BL_MINRAM<<30) - ((i64)256<<20)) {
        return "This computer has too little memory for the built-in model, "
               "which needs 6 GB.";
    }
    Str file = os_guimap(os, scratch, b->path);
    if (!file.len) {
        Buf m(&scratch);
        print(&m, "The built-in model is not downloaded yet: its file would be ");
        print(&m, b->path);
        print(&m, ". Download it (3.3 GB) from Settings, endpoint ");
        print(&m, c->endpoint);
        print(&m, ".");
        return finish(&m);
    }

    if (!b->pool) {
        Region *r = os_reserve(os, (iz)256<<20);
        if (!r) {
            os_guiunmap(os, file);
            return "Out of memory for the built-in model.";
        }
        Arena pa = newarena(r);  // the pool's for good
        i32 threads = gmthreads(os_guicores(os, scratch));
        Str env     = os_getenv(os, &scratch, "PROSPERO_BUILTIN_THREADS");
        Int n       = parseint(env);
        threads     = n.ok && n.val>0 && n.val<=256 ? (i32)n.val : threads;
        os_guithreadfast(os);
        b->pool = gmpool(os, &pa, threads, (iz)GM_CONTEXT>(iz)GM_WIDTH ? (iz)GM_CONTEXT : (iz)GM_WIDTH,
                         os_guithreadfast);
    }

    Region *mem = os_reserve(os, (iz)4<<30);
    if (!mem) {
        os_guiunmap(os, file);
        return "Out of memory for the built-in model.";
    }
    b->model   = newarena(mem);
    GmLoaded m = gmload(b->pool, &b->model, scratch, file, GM_CONTEXT, isa);
    if (!m.g) {
        os_release(os, mem);
        os_guiunmap(os, file);
        b->model = {};
        return concat(&scratch, "The built-in model's file is unusable: ", m.err);
    }
    // The file stays mapped: the model reads its tokenizer's text there
    i64 budget = ram ? ram/8 : (i64)1<<30;
    budget  = budget<((i64)2<<30) ? budget : (i64)2<<30;
    b->g    = m.g;
    b->chat = gmchatnew(os, &b->model, m.g, GM_CONTEXT, budget, os_random(os));
    return {};
}

// One event of a fake answer's stream: a delta of key, or with key
// null the end, with usage.
static Str blfakeevent(Arena *a, Str key, Str text)
{
    Buf     b(a, 512 + text.len);
    JsonOut w = {&b};
    print(&b, "data: ");
    putobject(&w);
    putstrkey(&w, "id", "chatcmpl-fake");
    putstrkey(&w, "object", "chat.completion.chunk");
    putstrkey(&w, "model", gmmodelid);
    putkey(&w, "choices");
    putarray(&w);
    putobject(&w);
    putintkey(&w, "index", 0);
    putkey(&w, "delta");
    putobject(&w);
    if (key.len) {
        putstrkey(&w, key, text);
    }
    putend(&w);
    if (!key.len) {
        putstrkey(&w, "finish_reason", "stop");
    }
    putend(&w);
    putend(&w);
    if (!key.len) {
        putkey(&w, "usage");
        putobject(&w);
        putintkey(&w, "prompt_tokens", 100);
        putintkey(&w, "completion_tokens", 10);
        putintkey(&w, "total_tokens", 110);
        putend(&w);
    }
    putend(&w);
    print(&b, "\n\n");
    return finish(&b);
}

// PROSPERO_TEST_BUILTIN=fake's answer to every chat, without the model:
// thoughts, then words, slowly enough to wait in line behind, and to
// cancel, as the model's own stream would come.
static void blfake(Builtin *b, BlCall *c, Arena scratch)
{
    static Str const words[] = {
        "Hello", " from", " the", " built-in", " model.",
        " It", " runs", " on", " this", " computer.",
    };
    BlEmit e = {b, c};
    blpost(b, c, blhead(&scratch, c, 200, "text/event-stream"));
    blemit(&e, blfakeevent(&scratch, "reasoning_content", "A greeting."));
    for (iz i = 0; i < countof(words); i++) {
        os_wait(&b->nap, 500);
        if (__atomic_load_n(&c->cancel, __ATOMIC_RELAXED)) {
            return;
        }
        Arena t = scratch;
        blemit(&e, blfakeevent(&t, "content", words[i]));
    }
    blemit(&e, blfakeevent(&scratch, {}, {}));
    blemit(&e, "data: [DONE]\n\n");
    blpost(b, c, bldone(&scratch, c, "ok", {}));
}

static void blrun(Builtin *b, BlCall *c, Arena scratch)
{
    Os   *os   = b->os;
    Arena work = newarena(os_reserve(os, (iz)2<<30));
    if (!work.beg) {
        blanswer(b, c, &scratch, 500, gmerrbody(&scratch, "Out of memory for the built-in model."));
        return;
    }
    os_guibusy(os, 1);
    if (b->fake) {
        blfake(b, c, scratch);
        os_guibusy(os, 0);
        os_release(os, work.region);
        return;
    }

    Str err = blload(b, c, scratch);
    JsonDoc d = {};
    if (!err.len) {
        d   = jsonparse(&scratch, Str(c->body, c->len));
        err = d.ok && d.root->type==JSON_OBJECT ? Str{} : Str("The request is not a JSON object.");
    }
    if (err.len) {
        blanswer(b, c, &scratch, 400, gmerrbody(&scratch, err));
    } else if (!__atomic_load_n(&c->cancel, __ATOMIC_RELAXED)) {
        GmTurn t = gmturn(b->chat, &scratch, work, d.root, c->len);
        if (t.status) {
            blanswer(b, c, &scratch, t.status, t.err);
        } else {
            blpost(b, c, blhead(&scratch, c, 200, "text/event-stream"));
            BlEmit e   = {b, c};
            GmOut  out = {blemit, &e, &c->cancel};
            gmgenerate(b->chat, work, &t, c->sid, d.root, &out);
            if (!__atomic_load_n(&c->cancel, __ATOMIC_RELAXED)) {
                blpost(b, c, bldone(&scratch, c, "ok", {}));
            }
        }
    }
    os_guibusy(os, 0);
    os_release(os, work.region);
}

// The next call queued, waiting for one, as the model's thread's.
static BlCall *bltake(Builtin *b)
{
    for (;;) {
        BlCall *next = 0;
        os_lock(&b->lock);
        for (i32 i = 0; i < BL_CALLS; i++) {
            BlCall *c = b->calls + i;
            if (c->state==BL_QUEUED && (!next || c->seq<next->seq)) {
                next = c;
            }
        }
        if (next) {
            next->state = BL_RUNNING;
        }
        os_unlock(&b->lock);
        if (next) {
            return next;
        }
        os_wait(&b->ready, -1);
    }
}

static void blthread(void *arg)
{
    Builtin *b       = (Builtin *)arg;
    Region  *mem     = os_reserve(b->os, (iz)1<<30);
    b->emitmem       = os_reserve(b->os, (iz)4<<20);
    if (!mem || !b->emitmem) {
        os_oom();
    }
    for (;;) {
        BlCall *c = bltake(b);
        if (!__atomic_load_n(&c->cancel, __ATOMIC_RELAXED)) {
            blrun(b, c, newarena(mem));
        }
        os_release(b->os, c->mem);
        mailpost(b->out, BL_FREE, (u64)(c - b->calls), {}, 1);
    }
}


// The UI thread's

static Builtin *blget(Gui *g)
{
    if (g->bl) {
        return g->bl;
    }
    Builtin *b = alloc<Builtin>(&g->perm);
    b->os  = g->os;
    b->out = newmailbox(&g->perm, BL_MAIL, wakegui, g->os);
    os_lockinit(&b->lock);
    if (!b->out || !os_eventinit(&b->ready) || !os_eventinit(&b->nap)) {
        return 0;
    }
    Arena t   = g->frame;
    Str   env = os_getenv(g->os, &t, "PROSPERO_BUILTIN_MODEL");
    b->path   = env.len ? clone(&g->perm, env) :
                concat(&g->perm, concat(&t, g->statedir, "/models/"), blfile);
    b->fake   = os_getenv(g->os, &t, "PROSPERO_TEST_BUILTIN") == "fake";
    g->bl     = b;
    return b;
}

// Offer the configuration's builtin endpoints to the main engine, if
// that is news to it: force after it came up anew.
static void bloffer(Gui *g, b32 force)
{
    if (g->enginekind==ENGINE_FAKE || !engineup(g) || !hostcan(g->main, "relay.offer")) {
        return;
    }
    Arena   a = newarena(g->cmdr);
    Buf     o(&a, 1024);
    JsonOut w = {&o};
    b32     any = 0;
    putobject(&w);
    putstrkey(&w, "machine", "");
    putkey(&w, "endpoints");
    putarray(&w);
    for (iz i = 0; i < g->cfgrows.len; i++) {
        CfgRow *r = &g->cfgrows[i];
        if (r->kind=="endpoint" && r->key=="kind" && trim(r->value)=="builtin" &&
                !startswith(r->file, "relayed from ")) {
            putobject(&w);
            putstrkey(&w, "name", r->name);
            putstrkey(&w, "kind", "builtin");
            putkey(&w, "models");
            putarray(&w);
            putstr(&w, gmmodelid);
            putend(&w);
            putend(&w);
            any = 1;
        }
    }
    putend(&w);
    putend(&w);
    Str offer = finish(&o);

    Builtin *b = any || (g->bl && g->bl->offering) ? blget(g) : 0;
    if (!b || (!force && b->offered.data && b->offered==offer)) {
        return;
    }
    if (!b->offermem) {
        b->offermem = os_reserve(g->os, (iz)1<<20);
    }
    if (b->offermem && offer.len<=((iz)1<<20)-64) {
        Arena m = newarena(b->offermem);
        b->offered = clone(&m, offer);
    }
    b->offering = any;

    Buf   c(&a, 256 + offer.len);
    EvOut e = cmdopen(g, &c, CMD_RELAYOFFER, 0, g->main);
    putintkey(&e.json, "peer", (i64)BL_SELF);  // replaced by a host's
    putkey(&e.json, "offer");
    putraw(&e.json, offer);
    cmdsend(g, &e, a);
}

// Main engine came up (again): what it had is gone.
static void blhello(Gui *g)
{
    if (g->bl) {
        g->bl->offered = {};
    }
    bloffer(g, 1);
}

static void blsend(Gui *g, Str line)
{
    guimainline(g, newarena(g->cmdr), line);
}

static BlCall *blcall(Builtin *b, i64 rid)
{
    for (i32 i = 0; rid>0 && i < BL_CALLS; i++) {
        BlCall *c = b->calls + i;
        if (c->state && !c->dead && c->rid==rid) {
            return c;
        }
    }
    return 0;
}

// The call ends here: its slot is free now, or once the model's thread
// lets go of it, its lines going nowhere.
static void blend(Builtin *b, BlCall *c)
{
    os_lock(&b->lock);
    if (c->state == BL_RUNNING) {
        __atomic_store_n(&c->cancel, 1, __ATOMIC_RELAXED);
        c->dead = 1;
    } else {
        if (c->mem) {
            os_release(b->os, c->mem);
        }
        *c = {};
    }
    os_unlock(&b->lock);
}

// The call can't be taken: lost, with why.
static void bllost(Gui *g, Arena a, i64 rid, i64 peer, Str why)
{
    BlCall c = {};
    c.rid  = rid;
    c.peer = peer;
    blsend(g, bldone(&a, &c, "lost", why));
}

// A model listing, as llama-server gives it, with the context window.
static Str bllisting(Arena *a)
{
    Buf     b(a, 256);
    JsonOut w = {&b};
    putobject(&w);
    putkey(&w, "data");
    putarray(&w);
    putobject(&w);
    putstrkey(&w, "id", gmmodelid);
    putstrkey(&w, "object", "model");
    putintkey(&w, "max_input_tokens", GM_CONTEXT);
    putstrkey(&w, "owned_by", "prospero");
    putend(&w);
    putend(&w);
    putend(&w);
    return finish(&b);
}

// The main engine's relay record, if it is for the built-in model: part
// of a call, or its cancelling. Returns 0 if it is another's.
static b32 blrelayin(Gui *g, Arena a, Json *j)
{
    i64 rid  = jsonint(jsonget(j, "rid"), 0);
    i64 peer = jsonint(jsonget(j, "peer"), 0);
    i64 part = jsonint(jsonget(j, "part"), -1);
    Builtin *b = g->bl;
    BlCall  *c = b ? blcall(b, rid) : 0;
    if (jsonflag(jsonget(j, "cancel"), 0)) {
        if (c) {
            blend(b, c);
        }
        return c != 0;
    } else if (!c && (part || !(jsonstr(jsonget(j, "kind")) == "builtin"))) {
        return 0;
    }

    if (!c) {
        b = blget(g);
        for (i32 i = 0; b && !c && i < BL_CALLS; i++) {
            c = b->calls[i].state ? 0 : b->calls+i;
        }
        i64 len = jsonint(jsonget(j, "len"), -1);
        Str why = !b ? Str("the window can't run the built-in model") :
                  !c ? Str("the built-in model has too many calls at once") :
                  len<0 || len>relaybodymax() ? Str("the request is too large") :
                  jsonint(jsonget(j, "zlen"), 0) || jsonget(j, "base")->type!=JSON_MISSING ?
                       Str("the request came packed, which the built-in model takes no more") :
                  Str{};
        Region *mem = why.len ? 0 : os_reserve(g->os, (iz)len + 4096);
        why = why.len || mem ? why : Str("out of memory");
        if (why.len) {
            bllost(g, a, rid, peer, why);
            return 1;
        }
        Arena m = newarena(mem);
        Str   path   = jsonstr(jsonget(j, "path"));
        Str   method = jsonstr(jsonget(j, "method"));
        *c = {};
        c->mem      = mem;
        c->rid      = rid;
        c->peer     = peer;
        c->sid      = clone(&m, jsonstr(jsonget(j, "session")));
        c->endpoint = clone(&m, jsonstr(jsonget(j, "endpoint")));
        c->len      = (iz)len;
        c->body     = alloc<u8>(&m, c->len);
        c->state    = BL_PARTS;
        // What it asks for, until its body is in: 1 a listing, 2 a chat
        c->seq      = method=="GET" && endswith(path, "models") ? 1 :
                      method=="POST" && endswith(path, "chat/completions") ? 2 : 0;
    }

    Unbase64 d = unbase64(&a, jsonstr(jsonget(j, "data")));
    if (!d.ok || d.data.len>c->len-c->got) {
        bllost(g, a, rid, c->peer, "the request was relayed unsound");
        blend(b, c);
        return 1;
    }
    if (d.data.len) {
        copybytes(c->body+c->got, d.data.data, d.data.len);
    }
    c->got += d.data.len;
    i64 parts = jsonint(jsonget(j, "parts"), 1);
    if (part+1 < parts) {
        Buf m(&a, 64);
        print(&m, "{\"cmd\":\"relay.more\",\"rid\":");
        print(&m, rid);
        putbyte(&m, '}');
        blsend(g, finish(&m));  // a host asks for itself
        return 1;
    } else if (c->got != c->len) {
        bllost(g, a, rid, c->peer, "the request was relayed unsound");
        blend(b, c);
        return 1;
    }

    switch (c->seq) {
    case 1:
        blsend(g, blhead(&a, c, 200, "application/json"));
        blsend(g, blbody(&a, c, bllisting(&a)));
        blsend(g, bldone(&a, c, "ok", {}));
        blend(b, c);
        return 1;
    case 0:
        blsend(g, blhead(&a, c, 404, "application/json"));
        blsend(g, blbody(&a, c, gmerrbody(&a, "The built-in model serves only "
                                              "v1/models and v1/chat/completions.")));
        blsend(g, bldone(&a, c, "ok", {}));
        blend(b, c);
        return 1;
    }

    if (!b->started) {
        b->started = os_thread(g->os, blthread, b);
        if (!b->started) {
            bllost(g, a, rid, c->peer, "the built-in model's thread can't start");
            blend(b, c);
            return 1;
        }
    }
    os_lock(&b->lock);
    c->seq   = ++b->seq;
    c->state = BL_QUEUED;
    os_unlock(&b->lock);
    os_signal(&b->ready);
    return 1;
}

// Lines from the model's thread to the main engine, as it takes them.
// Returns whether some wait for room.
static b32 blflush(Gui *g)
{
    Builtin *b = g->bl;
    if (!b) {
        return 0;
    }
    for (;;) {
        if (b->pend.off == b->pend.len) {
            b->pend = mailtake(b->out);
            if (!b->pend.len) {
                return 0;
            }
        }
        iz     at = b->pend.off;
        Record r  = mailnext(&b->pend);
        if (r.id >= (u64)BL_CALLS) {
            continue;
        }
        BlCall *c = b->calls + r.id;
        if (r.type == BL_FREE) {
            os_lock(&b->lock);
            *c = {};  // the thread released its memory
            os_unlock(&b->lock);
        } else if (!c->dead && !guimainline(g, newarena(g->cmdr), r.data)) {
            b->pend.off = at;
            return 1;
        }
    }
}

// The window goes to the background: every call is lost, and the engine
// told, unless a host tells it, which it does for a client that goes.
static void blgone(Gui *g)
{
    Builtin *b = g->bl;
    if (!b) {
        return;
    }
    for (i32 i = 0; i < BL_CALLS; i++) {
        if (b->calls[i].state) {
            blend(b, b->calls+i);
        }
    }
    if (b->offering && g->main->kind==GH_ENGINE) {
        Arena a = newarena(g->cmdr);
        Buf   m(&a, 128);
        print(&m, "{\"cmd\":\"relay.peer\",\"peer\":");
        print(&m, (i64)BL_SELF);
        print(&m, ",\"state\":\"gone\"}");
        blsend(g, finish(&m));
    }
    b->offered = {};
}
