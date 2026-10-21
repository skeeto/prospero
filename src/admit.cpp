// Admission: model requests wait their turn at each endpoint
//
// Every model request, a step of a turn or a compaction, is admitted at
// its endpoint before it goes out. An endpoint has one queue, shared by
// all sessions, and three limits:
//
// - requests in flight at once (maxreq, 0 for no limit);
// - different models in flight at once (maxmodels, 0 for no limit): a
//   llama.cpp router with --models-max N keeps N models loaded, and a
//   request for another evicts one, so requests for more models than
//   that at once would load and unload models over and over;
// - each model's own concurrency (a waiter's cap: the server's slots).
//
// Only counted models take one of the endpoint's model places: those a
// router loads itself, not the remote ones it only relays.
//
// The queue is first come, first served, with one exception that keeps
// a loaded model busy: a request for a model already in use may pass a
// waiter blocked on another model, until that waiter has waited
// ADM_PATIENCE. A model also stays in use for ADM_LINGER after a request
// that ends mid-turn, so that a session running a quick tool between
// steps keeps its model rather than lose it to a waiter, and pay for two
// loads; a waiter past its patience ignores that, and a model that takes
// a place ends the lingering of the rest, which the router evicts. Past
// its patience, nothing passes a waiter any more, and it goes once the
// requests in flight have ended. Two
// sessions on different models of a one-model router so take turns of
// about ADM_PATIENCE each, rather than a model load at every step.
//
// This is pure: the engine sends what is admitted, and wakes up for
// admnext to admit what time alone lets go.
//
// This is free and unencumbered software released into the public domain.

enum : i64 {
    ADM_LINGER   = 10*(i64)1000000,   // os_ticks
    ADM_PATIENCE = 120*(i64)1000000,
};

enum : i32 {
    PROBE_NONE,     // the endpoint's capacity was never asked
    PROBE_RUNNING,
    PROBE_DONE,
};

// A model at an endpoint, once it was admitted there.
struct AdmLane {
    AdmLane *next;
    Str      model;
    i32      inflight;
    b32      counted;   // takes one of the endpoint's model places
    i64      warm;      // in use until then, even with nothing in flight
};

struct AdmWait {
    AdmWait *next;
    u64      id;        // the waiter's, as the caller knows it
    Str      model;     // must outlive the wait
    i32      cap;       // the model's concurrency
    b32      counted;
    i64      since;     // os_ticks
};

struct AdmPoint {
    AdmPoint *next;
    Str       url;
    i32       maxreq;     // limits, set by the engine before admitting
    i32       maxmodels;
    i32       cfgmodels;  // maxmodels as configured, 0 if not
    i32       inflight;
    AdmLane  *lanes;
    AdmWait  *head;       // the queue, in arrival order
    AdmWait  *spare;      // waits to reuse
    // The engine's question to the endpoint about its capacity
    i32       probe;      // PROBE_*
    u64       probeid;
    Http     *http;
    i32       status;
    Buf       body;
    i32       models;     // a router's max_instances, 0 if not a router
    i32       slots;      // a server's total_slots, 0 if unknown
};

struct Adm {
    AdmPoint *points;
};

static AdmPoint *admpoint(Adm *adm, Arena *perm, Str url)
{
    for (AdmPoint *p = adm->points; p; p = p->next) {
        if (p->url == url) {
            return p;
        }
    }
    AdmPoint *p = alloc<AdmPoint>(perm);
    p->url  = clone(perm, url);
    p->next = adm->points;
    adm->points = p;
    return p;
}

// A model's lane, added if perm is given, else null if never admitted.
static AdmLane *admlane(AdmPoint *p, Arena *perm, Str model)
{
    for (AdmLane *l = p->lanes; l; l = l->next) {
        if (l->model == model) {
            return l;
        }
    }
    if (!perm) {
        return 0;
    }
    AdmLane *l = alloc<AdmLane>(perm);
    l->model = clone(perm, model);
    l->next  = p->lanes;
    p->lanes = l;
    return l;
}

// Requests in flight, or lingering after one, unless strict.
static b32 adminuse(AdmLane *l, i64 now, b32 strict)
{
    return l->inflight>0 || (!strict && l->warm>now);
}

// Whether a waiter fits the limits now, with strict ignoring lingering.
static b32 admfits(AdmPoint *p, AdmWait *w, i64 now, b32 strict)
{
    if (p->maxreq && p->inflight>=p->maxreq) {
        return 0;
    }
    AdmLane *l = admlane(p, 0, w->model);
    if (l && l->inflight>=w->cap) {
        return 0;
    } else if (!w->counted || !p->maxmodels || (l && adminuse(l, now, strict))) {
        return 1;  // takes no new model place
    }
    i32 used = 0;
    for (AdmLane *o = p->lanes; o; o = o->next) {
        used += o!=l && o->counted && adminuse(o, now, strict);
    }
    return used < p->maxmodels;
}

static void admenqueue(AdmPoint *p, Arena *perm, u64 id, Str model, i32 cap,
                       b32 counted, i64 now)
{
    AdmWait *w = p->spare;
    if (w) {
        p->spare = w->next;
    } else {
        w = alloc<AdmWait>(perm);
    }
    *w = {};
    w->id      = id;
    w->model   = model;
    w->cap     = cap>0 ? cap : 1;
    w->counted = counted;
    w->since   = now;
    AdmWait **tail = &p->head;
    for (; *tail; tail = &(*tail)->next) {}
    *tail = w;
}

// Admit one waiter that may go now, in turn, counting its request in
// flight. Returns its id, or 0 if none may go.
static u64 admgo(AdmPoint *p, Arena *perm, i64 now)
{
    AdmWait *blocked = 0;  // the first waiter that cannot go
    for (AdmWait **prev = &p->head; *prev; prev = &(*prev)->next) {
        AdmWait *w  = *prev;
        b32      ok = 0;
        if (!blocked) {
            ok = admfits(p, w, now, now-w->since >= ADM_PATIENCE);
        } else if (now-blocked->since < ADM_PATIENCE) {
            AdmLane *l = admlane(p, 0, w->model);
            ok = l && adminuse(l, now, 0) && admfits(p, w, now, 0);
        } else {
            return 0;  // all wait in line behind it
        }
        if (!ok) {
            blocked = blocked ? blocked : w;
            continue;
        }
        AdmLane *l = admlane(p, perm, w->model);
        if (w->counted && p->maxmodels && !adminuse(l, now, 0)) {
            // It takes a place: the router evicts what only lingered
            for (AdmLane *o = p->lanes; o; o = o->next) {
                o->warm = o->inflight ? o->warm : 0;
            }
        }
        l->inflight++;
        l->counted = w->counted;
        p->inflight++;
        *prev    = w->next;
        w->next  = p->spare;
        p->spare = w;
        return w->id;
    }
    return 0;
}

// A request admitted earlier ended. With linger, its model stays in
// use a while for the session's next request.
static void admdone(AdmPoint *p, Str model, i64 now, b32 linger)
{
    AdmLane *l = admlane(p, 0, model);
    assert(l && l->inflight>0 && p->inflight>0);
    l->inflight--;
    p->inflight--;
    if (linger && l->warm<now+ADM_LINGER) {
        l->warm = now + ADM_LINGER;
    }
}

// A session needs its model no longer (its turn ended): stop lingering.
static void admcool(AdmPoint *p, Str model)
{
    AdmLane *l = admlane(p, 0, model);
    if (l && !l->inflight) {
        l->warm = 0;
    }
}

// Leave the queue without being admitted.
static void admleave(AdmPoint *p, u64 id)
{
    for (AdmWait **prev = &p->head; *prev; prev = &(*prev)->next) {
        AdmWait *w = *prev;
        if (w->id == id) {
            *prev    = w->next;
            w->next  = p->spare;
            p->spare = w;
            return;
        }
    }
}

// A waiter's place in the queue, from 1, or 0 if not waiting.
static i32 admpos(AdmPoint *p, u64 id)
{
    i32 n = 1;
    for (AdmWait *w = p->head; w; w = w->next, n++) {
        if (w->id == id) {
            return n;
        }
    }
    return 0;
}

static i32 admlen(AdmPoint *p)
{
    i32 n = 0;
    for (AdmWait *w = p->head; w; w = w->next) {
        n++;
    }
    return n;
}

// When time alone may let a waiter go: a lingering model's end, or the
// end of the first waiter's patience. 0 if nothing waits for time.
static i64 admnext(AdmPoint *p, i64 now)
{
    if (!p->head) {
        return 0;
    }
    i64 next = p->head->since + ADM_PATIENCE;
    next = next>now ? next : 0;
    for (AdmLane *l = p->lanes; l; l = l->next) {
        if (!l->inflight && l->warm>now && (!next || l->warm<next)) {
            next = l->warm;
        }
    }
    return next;
}
