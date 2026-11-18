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
    BL_TICK = 3,  // the download's news (blnews)
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
    struct BlDown *down;  // the download, once one starts
    i64      statat;   // when have and partlen were looked at
    b32      have;     // the file is there
    i64      partlen;  // bytes of a download to resume
    b32      applied;  // a download's end was seen
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

// Why the call can't be answered: the file isn't there.
static Str blmissing(Builtin *b, BlCall *c, Arena *a)
{
    Buf m(a);
    print(&m, "The built-in model is not downloaded yet: its file would be ");
    print(&m, b->path);
    print(&m, ". Download it (3.3 GB) from Settings, on endpoint ");
    print(&m, c->endpoint);
    print(&m, "'s page.");
    return finish(&m);
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
        return blmissing(b, c, &scratch);
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
        // Its file there, if only a stand-in
        if (os_stat(os, scratch, b->path).type == STAT_FILE) {
            blfake(b, c, scratch);
        } else {
            Str why = blmissing(b, c, &scratch);
            blanswer(b, c, &scratch, 400, gmerrbody(&scratch, why));
        }
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

static void blnews(Gui *);

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
        if (r.type == BL_TICK) {
            blnews(g);
            continue;
        } else if (r.id >= (u64)BL_CALLS) {
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


// The model's file, downloaded
//
// From Hugging Face, at a pinned revision, into the state directory's
// models folder. It arrives in FILE.part, hashed as it comes, and takes
// its name only once all of it is there and its SHA-256 is the one
// pinned here. A download cancelled, or cut off, resumes from the part
// (Range), hashing what is there first; a server that answers a resume
// with the whole file starts it over. Redirects are followed by hand
// (Hugging Face's go to its CDN), never from https to http, at most 5.
//
// The URL, size and hash can be another's for tests:
// PROSPERO_TEST_BUILTIN_URL, _SIZE and _SHA256.

static Str const blurl =
    "https://huggingface.co/wellons/gemma-4-E2B-it-qat-q4_0-refit-GGUF/resolve/"
    "4bf900f6bf9617f9edf36aaf8b727eaec9fc5861/gemma-4-E2B-it-qat-q4_0-refit.gguf";
static Str const blsha =
    "ad5ae441f8efc3f5588a9567ff6cc31d99e104f94815996fc2dae0ff6d06cf28";

enum : i64 {
    BL_SIZE  = 3349516256,
    BL_SLACK = (i64)64 << 20,  // disk space to spare beyond the file
};

enum : i32 {
    BL_IDLE,      // no download since the window opened, or it was taken
    BL_FETCHING,
    BL_FAILED,    // err says why, a cancel among them
    BL_FETCHED,   // the file is in place
};

struct BlDown {
    Os      *os;
    Mailbox *out;      // the window's, for BL_TICK
    Mailbox *mb;       // HTTP records
    Str      url;
    Str      sha;
    Str      path;
    Str      part;
    i64      size;
    i64      got;      // atomic: bytes there so far
    i32      state;    // atomic
    i32      cancel;   // atomic
    Lock     lock;
    Http    *http;     // the request in flight, under the lock
    i64      tickat;   // the thread's: when it last told the window
    u8       err[512];
    iz       errlen;   // set before state goes BL_FAILED
};

static void bltick(BlDown *d, b32 now)
{
    i64 t = os_ticks(d->os);
    if (now || t-d->tickat>=200000) {
        d->tickat = t;
        mailpost(d->out, BL_TICK, 0, {}, now);
    }
}

static b32 blcancelled(BlDown *d)
{
    return __atomic_load_n(&d->cancel, __ATOMIC_RELAXED);
}

// Hash the part already there, which a resume goes on from, counting it
// as got. Returns its length, 0 if it is unusable (and then gone).
static i64 blrehash(BlDown *d, Arena scratch, Sha256 *h)
{
    Os  *os = d->os;
    Stat st = os_stat(os, scratch, d->part);
    if (st.type!=STAT_FILE || !st.size) {
        return 0;
    } else if (st.size > d->size) {
        os_remove(os, scratch, d->part);
        return 0;
    }
    Str m = os_guimap(os, scratch, d->part);
    if (m.len != st.size) {
        if (m.len) {
            os_guiunmap(os, m);
        }
        return 0;
    }
    for (iz off = 0; off < m.len && !blcancelled(d);) {
        iz n = m.len-off<((iz)16<<20) ? m.len-off : (iz)16<<20;
        sha256put(h, Str(m.data+off, n));
        off += n;
        __atomic_store_n(&d->got, (i64)off, __ATOMIC_RELAXED);
        bltick(d, 0);
    }
    os_guiunmap(os, m);
    return st.size;
}

// One request for the rest, from have on, its body appended to the log,
// into the hash. Its status, and where a redirect points.
struct BlGot {
    i32 status;
    Str location;
    Str err;
    b32 restart;  // a whole file in answer to a resume: started over
};

static BlGot blget(BlDown *d, Arena *perm, Arena scratch, Str url, iz log,
                   Sha256 *h, i64 *have, u64 id)
{
    Os   *os = d->os;
    BlGot r  = {};
    Buf   hb(&scratch, 128);
    print(&hb, "User-Agent: Prospero\r\n");
    if (*have) {
        print(&hb, "Range: bytes=");
        print(&hb, *have);
        print(&hb, "-\r\n");
    }
    HttpReq q = {};
    q.method    = "GET";
    q.url       = url;
    q.headers   = finish(&hb);
    q.connectms = 15000;
    q.readms    = 60000;
    q.maxbytes  = d->size + ((i64)1<<20);
    q.mbox      = d->mb;
    q.id        = id;
    Http *http = os_http(os, &q, scratch);
    if (!http) {
        r.err = concat(perm, "could not connect: ", os_lasterr(os, &scratch));
        return r;
    }
    os_lock(&d->lock);
    d->http = http;
    os_unlock(&d->lock);
    if (blcancelled(d)) {
        os_httpcancel(os, http);
    }

    b32 ok   = 0;  // the body is the file's
    b32 done = 0;
    while (!done) {
        mailwait(d->mb, -1);
        Mail m = mailtake(d->mb);
        for (Record rec = mailnext(&m); rec.ok && !done; rec = mailnext(&m)) {
            if (rec.id != id) {
                continue;  // an earlier request's, given up on
            }
            switch (rec.type) {
            case HTTP_HEAD: {
                r.status = get32(rec.data);
                Str range = {};
                for (Str rest = cuthead(rec.data, 4); rest.len;) {
                    Cut line = cut(rest, '\n');
                    Cut kv   = cut(line.head, ':');
                    rest = line.tail;
                    if (kv.ok && equalfold(trim(kv.head), "location")) {
                        r.location = clone(perm, trim(kv.tail));
                    } else if (kv.ok && equalfold(trim(kv.head), "content-range")) {
                        range = trim(kv.tail);
                    }
                }
                if (r.status==200 && *have) {
                    // The whole file again: over from the start
                    if (!os_logtrunc(os, log, 0)) {
                        r.err = "could not start the file over";
                        os_httpcancel(os, http);
                        break;
                    }
                    *h        = sha256init();
                    *have     = 0;
                    r.restart = 1;
                    ok        = 1;
                } else if (r.status == 200) {
                    ok = 1;
                } else if (r.status == 206) {
                    Buf want(&scratch, 64);
                    print(&want, "bytes ");
                    print(&want, *have);
                    putbyte(&want, '-');
                    ok = startswith(range, finish(&want));
                    if (!ok) {
                        r.err = "the server resumed from the wrong place";
                        os_httpcancel(os, http);
                    }
                }
            } break;
            case HTTP_BODY:
                if (!ok || r.err.len) {
                    break;
                } else if (rec.data.len > d->size-*have) {
                    r.err = "the file is larger than it should be";
                    os_httpcancel(os, http);
                } else if (!os_logwrite(os, log, rec.data)) {
                    r.err = "could not write the file: is the disk full?";
                    os_httpcancel(os, http);
                } else {
                    sha256put(h, rec.data);
                    *have += rec.data.len;
                    __atomic_store_n(&d->got, *have, __ATOMIC_RELAXED);
                    bltick(d, 0);
                }
                break;
            case HTTP_DONE: {
                i32 err = get32(rec.data);
                done = 1;
                if (!r.err.len && err==HTTP_ECANCEL) {
                    r.err = "cancelled";
                } else if (!r.err.len && err!=HTTP_OK) {
                    Str why = trim(cuthead(rec.data, 4));
                    r.err = concat(perm, "the connection failed: ",
                                   why.len ? why : Str("no reason given"));
                }
            } break;
            }
        }
    }
    os_lock(&d->lock);
    d->http = 0;
    os_unlock(&d->lock);
    os_httpfree(os, http);
    r.err = r.err.len ? clone(perm, r.err) : r.err;
    return r;
}

// The download, start to end. Returns why it failed, or null.
static Str blfetch(BlDown *d, Arena *perm, Arena scratch)
{
    Os    *os   = d->os;
    Sha256 h    = sha256init();
    i64    have = blrehash(d, scratch, &h);
    if (blcancelled(d)) {
        return "cancelled";
    }
    iz log = os_logopen(os, scratch, d->part);
    if (log < 0) {
        return concat(perm, "could not write ", d->part);
    } else if (!os_logtrunc(os, log, have)) {
        os_logclose(os, log);
        return concat(perm, "could not write ", d->part);
    }

    Str url = d->url;
    Str err = {};
    for (i32 hops = 0;; hops++) {
        BlGot g = blget(d, perm, scratch, url, log, &h, &have, (u64)hops+1);
        b32   moved = g.status==301 || g.status==302 || g.status==303 ||
                      g.status==307 || g.status==308;
        Str   next  = moved && g.location.len ? iniurlresolve(perm, url, g.location) : Str{};
        if (g.err.len) {
            err = g.err;
        } else if (moved && hops==5) {
            err = "more than 5 redirects";
        } else if (moved && !next.data) {
            err = "redirected without an http or https URL";
        } else if (moved && startswith(url, "https:") && !startswith(next, "https:")) {
            err = "redirected from https to http";
        } else if (moved) {
            url = next;
            continue;
        } else if (g.status!=200 && g.status!=206) {
            Buf b(perm, 64);
            print(&b, "the server answered HTTP ");
            print(&b, (i64)g.status);
            err = finish(&b);
        }
        break;
    }
    os_logsync(os, log);
    os_logclose(os, log);
    if (err.len) {
        return err;
    } else if (have != d->size) {
        Buf b(perm, 128);
        print(&b, "the download ended early, ");
        printfloat(&b, (f64)have/1e9, 1);
        print(&b, " of ");
        printfloat(&b, (f64)d->size/1e9, 1);
        print(&b, " GB");
        return finish(&b);
    }
    Str sum = sha256hex(&scratch, h);
    if (!(sum == d->sha)) {
        os_remove(os, scratch, d->part);
        return "the file that came is not the one expected (its SHA-256 differs)";
    } else if (!os_rename(os, scratch, d->part, d->path)) {
        return concat(perm, "could not rename the file to ", d->path);
    }
    return {};
}

static void blfetchthread(void *arg)
{
    BlDown *d   = (BlDown *)arg;
    Region *mem = os_reserve(d->os, (iz)64<<20);
    Str     err = mem ? Str{} : Str("out of memory");
    if (mem) {
        Arena a       = newarena(mem);
        Arena scratch = carve(&a, (iz)48<<20);
        err = blfetch(d, &a, scratch);
    }
    iz n = err.len<countof(d->err) ? err.len : countof(d->err);
    if (n) {
        copybytes(d->err, err.data, n);
    }
    d->errlen = n;
    __atomic_store_n(&d->state, n ? BL_FAILED : BL_FETCHED, __ATOMIC_RELEASE);
    if (mem) {
        os_release(d->os, mem);
    }
    bltick(d, 1);
}


// The download, as the window shows it

// Why this computer can't run the model, or null if it can.
static Str blcannot(Gui *g)
{
    if (gmisa(os_guiarmdot(g->os)) == GM_SCALAR) {
        return "its processor lacks AVX2 (x86-64) or the dot product "
               "instructions (arm64)";
    }
    i64 ram = os_guiram(g->os);
    if (ram && ram<((i64)BL_MINRAM<<30) - ((i64)256<<20)) {
        return "it has less than 6 GB of memory";
    }
    return {};
}

static Str blpart(Arena *a, Builtin *b)
{
    return concat(a, b->path, ".part");
}

// Whether the file, or a part of it, is there, looked at again at most
// every 2 s.
static void blstat(Gui *g, Builtin *b)
{
    i64 now = os_ticks(g->os);
    if (b->statat && now-b->statat<2000000) {
        return;
    }
    b->statat = now;
    Arena a   = g->frame;
    Str   part = blpart(&a, b);
    Stat  f    = os_stat(g->os, a, b->path);
    Stat  p    = os_stat(g->os, a, part);
    b->have    = f.type == STAT_FILE;
    b->partlen = p.type==STAT_FILE ? p.size : 0;
}

static i32 blfetchstate(Builtin *b)
{
    return b->down ? __atomic_load_n(&b->down->state, __ATOMIC_ACQUIRE) : BL_IDLE;
}

// The test's stand-ins for the pinned file, if any.
static i64 bltestsize(Gui *g, Arena a)
{
    Int n = parseint(os_getenv(g->os, &a, "PROSPERO_TEST_BUILTIN_SIZE"));
    return n.ok && n.val>0 ? n.val : BL_SIZE;
}

static void bldownload(Gui *g)
{
    Builtin *b = blget(g);
    if (!b || blfetchstate(b)==BL_FETCHING) {
        return;
    }
    Arena   a = g->frame;
    BlDown *d = b->down;
    if (!d) {
        d = alloc<BlDown>(&g->perm);
        d->os  = g->os;
        d->out = b->out;
        d->mb  = newmailbox(&g->perm, (iz)1<<20, 0, 0);
        os_lockinit(&d->lock);
        Str url  = os_getenv(g->os, &a, "PROSPERO_TEST_BUILTIN_URL");
        Str sha  = os_getenv(g->os, &a, "PROSPERO_TEST_BUILTIN_SHA256");
        d->url   = url.len ? clone(&g->perm, url) : blurl;
        d->sha   = sha.len ? clone(&g->perm, sha) : blsha;
        d->size  = bltestsize(g, a);
        d->path  = b->path;
        d->part  = blpart(&g->perm, b);
        b->down  = d;
    }
    d->got    = 0;
    d->cancel = 0;
    d->errlen = 0;
    d->tickat = 0;

    Str why = !d->mb ? Str("out of memory") : Str{};
    Str dir = d->path;
    for (; dir.len && dir[dir.len-1]!='/' && dir[dir.len-1]!='\\'; dir.len--) {}
    if (!why.len && dir.len && !os_mkdirs(g->os, a, dir)) {
        why = concat(&a, "could not make ", dir);
    }
    blstat(g, b);
    i64 free = why.len ? 0 : os_guidiskfree(g->os, a, dir.len ? dir : Str("."));
    if (!why.len && free>=0 && free<d->size-b->partlen+BL_SLACK) {
        Buf m(&a, 128);
        print(&m, "too little disk space: it needs ");
        printfloat(&m, (f64)(d->size-b->partlen+BL_SLACK)/1e9, 1);
        print(&m, " GB, and ");
        printfloat(&m, (f64)free/1e9, 1);
        print(&m, " GB is free");
        why = finish(&m);
    }
    d->state = BL_FETCHING;
    if (!why.len && !os_thread(g->os, blfetchthread, d)) {
        why = "could not start the download's thread";
    }
    if (why.len) {
        iz n = why.len<countof(d->err) ? why.len : countof(d->err);
        copybytes(d->err, why.data, n);
        d->errlen = n;
        d->state  = BL_FAILED;
    }
    b->applied = 0;
    scheddirty(&g->sched);
}

static void blstopdown(Gui *g)
{
    BlDown *d = g->bl ? g->bl->down : 0;
    if (!d || blfetchstate(g->bl)!=BL_FETCHING) {
        return;
    }
    __atomic_store_n(&d->cancel, 1, __ATOMIC_RELAXED);
    os_lock(&d->lock);
    if (d->http) {
        os_httpcancel(g->os, d->http);
    }
    os_unlock(&d->lock);
}

// The configuration's builtin endpoint, or null.
static Str blendpoint(Gui *g)
{
    for (iz i = 0; i < g->cfgrows.len; i++) {
        CfgRow *r = &g->cfgrows[i];
        if (r->kind=="endpoint" && r->key=="kind" && trim(r->value)=="builtin") {
            return r->name;
        }
    }
    return {};
}

// The file is there: an endpoint for it, unless there is one.
static void bluse(Gui *g)
{
    if (blendpoint(g).len) {
        return;
    }
    Str name = "gemma";
    for (iz i = 0; i < g->cfgrows.len; i++) {
        CfgRow *r = &g->cfgrows[i];
        name = r->kind=="endpoint" && r->name==name ? Str("gemma-builtin") : name;
    }
    guiconfigset(g, "endpoint", name, "kind", "builtin");
}

// The download's news, from blflush: a frame to show it, and at its end
// the endpoint, once.
static void blnews(Gui *g)
{
    Builtin *b = g->bl;
    scheddirty(&g->sched);
    if (blfetchstate(b)!=BL_FETCHING && !b->applied) {
        b->applied = 1;
        b->statat  = 0;
        if (blfetchstate(b) == BL_FETCHED) {
            bluse(g);
        }
    }
}

static void blsize(Buf *b, i64 n)
{
    printfloat(b, (f64)n/1e9, 1);
}

// The model's state and what to do about it: download, its progress and
// Cancel, a failure and Retry or Resume, or Use once it is there. On
// the first-run card (card), else on a builtin endpoint's page.
static void bldrawfetch(Gui *g, Arena a, b32 card)
{
    Builtin *b = blget(g);
    if (!b) {
        return;
    }
    Str cannot = blcannot(g);
    if (cannot.len) {
        ImGui::PushStyleColor(ImGuiCol_Text, g->pal->dim);
        ImGui::TextWrapped("This computer can't run the built-in model: %s.",
                           cstr0(&a, cannot));
        ImGui::PopStyleColor();
        itemnamed(g, "builtin cannot");
        return;
    }
    i32 st = blfetchstate(b);
    if (st != BL_FETCHING) {
        blstat(g, b);
    }
    i64 size = b->down ? b->down->size : bltestsize(g, a);
    if (st == BL_FETCHING) {
        i64 got = __atomic_load_n(&b->down->got, __ATOMIC_RELAXED);
        Buf m(&a, 64);
        blsize(&m, got);
        print(&m, " of ");
        blsize(&m, size);
        print(&m, " GB");
        putbyte(&m, 0);
        f32 w = ImGui::GetContentRegionAvail().x;
        ImGui::ProgressBar(size ? (f32)((f64)got/(f64)size) : 0,
                           v2(w<360*g->scale ? w : 360*g->scale, 0),
                           (char const *)finish(&m).data);
        itemnamed(g, "builtin progress");
        if (ImGui::Button("Cancel##builtin")) {
            blstopdown(g);
        }
        itemnamed(g, "builtin cancel");
        itemtip("Stop the download, keeping what came, to resume later");
        return;
    }

    if (st==BL_FAILED && !(Str(b->down->err, b->down->errlen) == "cancelled")) {
        ImGui::PushStyleColor(ImGuiCol_Text, g->pal->bad);
        ImGui::TextWrapped("The download failed: %s.",
                           cstr0(&a, Str(b->down->err, b->down->errlen)));
        ImGui::PopStyleColor();
        itemnamed(g, "builtin failed");
    }
    if (b->have) {
        if (!card) {
            Buf m(&a, 256);
            print(&m, "Gemma 4 E2B is downloaded, at ");
            print(&m, b->path);
            print(&m, ".");
            putbyte(&m, 0);
            ImGui::PushStyleColor(ImGuiCol_Text, g->pal->dim);
            ImGui::TextWrapped("%s", (char const *)finish(&m).data);
            ImGui::PopStyleColor();
            itemnamed(g, "builtin have");
        } else if (ImGui::Button("Use the built-in model")) {
            bluse(g);
        }
        if (card) {
            itemnamed(g, "builtin use");
            itemtip("Add an endpoint for it, its file being here already");
        }
        return;
    }

    Buf m(&a, 64);
    if (b->partlen) {
        print(&m, "Resume the download (");
        blsize(&m, b->partlen);
        print(&m, " of ");
        blsize(&m, size);
        print(&m, " GB)");
    } else {
        print(&m, "Download Gemma 4 E2B (");
        blsize(&m, size);
        print(&m, " GB)");
    }
    putbyte(&m, 0);
    if (ImGui::Button((char const *)finish(&m).data)) {
        bldownload(g);
    }
    itemnamed(g, "builtin download");
    itemtip("Download the built-in model from Hugging Face into Prospero's "
            "state directory, its SHA-256 checked, then add an endpoint for it");
}

// On the first-run card, under Set up a model: the built-in model, for
// someone with no server or service yet.
static void bldrawcard(Gui *g, Arena a)
{
    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, g->pal->dim);
    ImGui::TextWrapped("Or try the built-in model, Gemma 4 E2B by Google "
                       "(Apache 2.0): free and private, it runs on this "
                       "computer, but it is slow, and far less capable than "
                       "the models of a service. A 3.3 GB download.");
    ImGui::PopStyleColor();
    bldrawfetch(g, a, 1);
}

// On a builtin endpoint's page in Settings.
static void bldrawpage(Gui *g, Arena a)
{
    ImGui::Spacing();
    bldrawfetch(g, a, 0);
}
