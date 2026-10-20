// Engine: sessions and the turn machine, on one thread
//
// The engine thread owns every session and runs one non-blocking state
// machine per session. It sleeps on its mailbox, which carries protocol
// commands from a frontend (MAIL_CMD lines), HTTP and process records
// from worker threads, and nothing else; timers are per-session deadline
// slots, and the wait is bounded by the earliest one. Output to the
// frontend is one mailbox record per protocol line (MAIL_OUT), posted
// without ever blocking: on overflow the frontend resyncs from the log.
//
// A turn: user input, then steps until the agent stops calling tools.
// Each step is one model response, which may take several attempts
// (HTTP requests). The rules that keep a turn from getting stuck hook
// into the transitions:
//
// - Watchdogs: first byte (30 min local, 300 s remote), idle 90 s (any
//   byte counts, ':' pings included), progress 600 s (content or
//   prompt progress). A watchdog firing is a transient failure.
// - One retry owner: transient failures before anything visible was
//   committed back off min(500 ms * 2^(n-1), 8 s) * U(0.75, 1), at most
//   10 times, honoring Retry-After up to 300 s and failing fast beyond.
// - Replay safety: once text or a tool call has streamed, the partial
//   response is committed and the agent is asked to continue instead.
// - Nudges, each with a cap and a counter reset by progress (a tool
//   that succeeded): continue, empty, length, loop, and the tool-call
//   loop guard (the same set of calls 5 steps in a row is not run).
// - Tools: denials and invalid calls are tool results, never the end
//   of a turn. Shell commands time out with TERM, 2 s, then KILL, and
//   whatever a command leaves behind is killed when it completes.
// - Budgets: a note at 200 steps, the end of the turn at 300.
//
// This is free and unencumbered software released into the public domain.

enum : u32 {
    MAIL_CMD  = 100,  // frontend to engine: one protocol command line
    MAIL_OUT,         // engine to frontend: one protocol record line
    MAIL_QUIT,        // engine to frontend: the engine has stopped
};

enum : i64 {
    MS           = 1000,          // in os_ticks units (microseconds)
    SEC          = 1000*MS,
    STEP_SOFT    = 200,
    STEP_HARD    = 300,
    RETRY_MAX    = 10,
    RETRY_WAIT   = 300*SEC,       // longest Retry-After honored
    GRACE        = 2500*MS,       // after finish_reason, for the usage chunk
    DRAIN        = 2*SEC,         // output after a command exits
    KILLWAIT     = 2*SEC,         // TERM to KILL
    RESPONSE_MAX = (i64)64 << 20,
    ERRBODY_MAX  = 64 << 10,
    MODELS_MAX   = 8 << 20,
    LOG_MAX      = (i64)1 << 30,
    SSE_MAX      = 16 << 20,      // longest line or event of a stream
};

enum : i32 {
    S_IDLE,
    S_DISCOVER,  // GET /v1/models
    S_HEAD,      // awaiting the response head
    S_STREAM,    // reading the response
    S_ERRBODY,   // reading an error response
    S_BACKOFF,   // waiting to retry
    S_TOOL,      // a command is running
};

static Str statenames[] = {
    "idle", "discover", "waiting", "streaming", "error", "backoff", "tool",
};

enum : i32 {
    T_FIRSTBYTE,
    T_IDLE,
    T_PROGRESS,
    T_GRACE,
    T_BACKOFF,
    T_TOOL,
    T_KILL,
    T_DRAIN,
    T_COUNT,
};

enum : i32 {
    N_CONTINUE,  // after an interrupted response
    N_EMPTY,     // nothing visible and no calls
    N_LENGTH,    // stopped at the length limit
    N_LOOP,      // a text cycle
    N_GUARD,     // the same calls 5 times
    N_COUNT,
};

static struct {
    Str name;
    i32 cap;
} nudgeinfo[] = {
    {"continue", 3},
    {"empty",    2},
    {"length",   3},
    {"loop",     3},
    {"guard",    2},
};
static_assert(countof(nudgeinfo) == N_COUNT);

struct Endpoint {
    Str url;
    Str key;        // bearer token, empty if none
    i64 firstbyte;  // watchdogs, in ticks
    i64 idle;
    i64 progress;
    i32 connectms;
};

struct Engine;

struct ShellRun {
    Str  spill;     // absolute path of the output file
    Str  rel;       // relative to the session directory
    i64  start;
    i64  timeout;   // seconds
    i64  total;
    i32  code;
    i32  signal;
    b32  exited;
    b32  eof;
    b32  timedout;
    b32  limited;
};

struct Session {
    Session     *next;
    Engine      *e;
    Arena        perm;      // the session's lifetime
    Region      *aregion;   // backs attempt
    Arena        attempt;   // reset for every HTTP request
    Str          sid;
    Str          dir;       // state/sessions/<sid>
    Str          outdir;    // dir/out
    Str          cwd;
    Str          model;
    Endpoint     ep;
    ModelConf    mc;
    ModelInfo    info;
    b32          discovered;  // this turn
    Str          system;
    Str          tools;
    ToolEnv      te;
    EventLog     log;
    Slice<Msg>   history;
    i32          state;
    i64          timers[T_COUNT];
    // The turn
    i64          turn;
    i64          steps;
    i64          attempts;
    i32          retries;    // for the current step
    i32          nudges[N_COUNT];
    u64          lastset;    // hash of the last step's calls
    i32          setrepeat;
    b32          blocked;    // this step's calls repeat too often
    b32          progressed; // a tool succeeded this step
    b32          contnext;   // nudge to continue after the tools
    b32          lengthcut;  // the response stopped at the length limit
    b32          cancelled;
    i64          used;       // tokens in the last request and response
    i64          added;      // bytes added to the history since
    Slice<Str>   queued;     // prompts waiting for the turn to end
    Slice<Str>   steers;     // input for the next step
    // The current request
    Http        *http;
    u64          httpid;
    i64          sent;       // ticks
    i32          status;
    Str          headers;
    Buf          body;       // error or discovery body, or a plain response
    b32          plain;      // a 200 without an event stream
    Sse          sse;
    Partial     *part;
    Cycle       *cycles;     // [0] text, [1] reasoning
    iz           cycleat;    // where the text to keep ends, after a cycle
    // Tools
    Slice<Block> calls;
    iz           callidx;
    i64          callstart;
    Proc        *proc;
    u64          procid;
    ShellRun     sh;
};

// Engine functions take scratch by value, so that whatever a caller has
// allocated in its own scratch stays intact below them.
struct Engine {
    Os         *os;
    Arena       perm;
    Region     *scratch;  // reset for every record and timer
    Region     *replay;   // log copies for replays, which need two arenas
    Mailbox    *in;
    Mailbox    *out;
    Ini        *ini;
    Str         statedir;
    Str         exedir;
    ToolEnv     te;
    Slice<Str>  env;      // inherited environment
    Session    *sessions;
    Map<i64>   *trained;  // n_ctx_train by "url model", learned when loaded
    u64         nextid;
    b32         quitting;
};




// Output

static void emit(Engine *e, Str line)
{
    mailpost(e->out, MAIL_OUT, 0, line, 0);
}

static EvOut sbegin(Session *s, Buf *b, i32 ev)
{
    if (evdurable(ev)) {
        return logbegin(&s->log, b, ev);
    }
    return evbegin(b, s->sid, 0, os_time(s->e->os), ev);
}

// Finish a record: durable ones go to the log first.
static void ssend(Session *s, EvOut *w)
{
    if (!evdurable(w->ev)) {
        emit(s->e, evend(w));
        return;
    }
    Str line = logappend(&s->log, w);
    if (!line.data) {
        // The log is broken (a full disk): keep the frontend informed
        line = cuthead(finish(w->json.b), w->start);
    }
    emit(s->e, line);
}

// Ticks as seconds, for messages.
static Str secstr(Arena *a, i64 ticks)
{
    Buf b(a, 24);
    printfloat(&b, (f64)ticks/(f64)SEC, 1);
    return finish(&b);
}

static void putstrkey(JsonOut *w, Str key, Str v)
{
    putkey(w, key);
    putstr(w, v);
}

static void putintkey(JsonOut *w, Str key, i64 v)
{
    putkey(w, key);
    putint(w, v);
}

static void note(Session *s, Arena scratch, Str level, Str text)
{
    Buf   b(&scratch, 256 + text.len);
    EvOut w = sbegin(s, &b, EV_NOTE);
    putstrkey(&w.json, "level", level);
    putstrkey(&w.json, "text", text);
    ssend(s, &w);
}

// The state for the frontend. Small and bounded, so it needs no scratch.
static void live(Session *s)
{
    byte  mem[512];
    Arena a = {mem, mem+countof(mem)};
    Buf   b(&a, 384);
    EvOut w = sbegin(s, &b, EV_LIVE);
    putstrkey(&w.json, "state", statenames[s->state]);
    if (s->state == S_BACKOFF) {
        i64 left = s->timers[T_BACKOFF] - os_ticks(s->e->os);
        putintkey(&w.json, "retry_at", os_time(s->e->os) + left/MS);
    }
    ssend(s, &w);
}

static void setstate(Session *s, i32 state)
{
    if (s->state != state) {
        s->state = state;
        live(s);
    }
}

static void arm(Session *s, i32 t, i64 delay)
{
    s->timers[t] = os_ticks(s->e->os) + delay;
}

static void disarm(Session *s, i32 t)
{
    s->timers[t] = 0;
}


// History

static void addmsg(Session *s, Msg m)
{
    s->history = push(&s->perm, s->history, m);
    s->added += m.text.len;
    for (iz i = 0; i < m.blocks.len; i++) {
        s->added += m.blocks[i].text.len + m.blocks[i].args.len;
    }
}

static void adduser(Session *s, Arena scratch, Str text, Str via)
{
    Buf   b(&scratch, 256 + text.len);
    EvOut w = sbegin(s, &b, EV_USER);
    putstrkey(&w.json, "text", text);
    putstrkey(&w.json, "via", via);
    ssend(s, &w);
    Msg m = {};
    m.role = ROLE_USER;
    m.text = clone(&s->perm, text);
    addmsg(s, m);
}

// Append an assistant record and message. The blocks are in perm.
static void addassistant(Session *s, Arena scratch, Slice<Block> blocks,
                         Str finishr, Usage *u)
{
    Buf   b(&scratch, 4096);
    EvOut w = sbegin(s, &b, EV_ASSISTANT);
    putintkey(&w.json, "turn", s->turn);
    putintkey(&w.json, "attempt", s->attempts);
    putkey(&w.json, "blocks");
    putarray(&w.json);
    for (iz i = 0; i < blocks.len; i++) {
        Block *k = &blocks[i];
        putobject(&w.json);
        putstrkey(&w.json, "t", blockname(k->type));
        if (k->type == BLOCK_CALL) {
            putstrkey(&w.json, "id", k->id);
            putstrkey(&w.json, "name", k->name);
            putstrkey(&w.json, "args", k->args);
        } else {
            putstrkey(&w.json, "text", k->text);
        }
        putend(&w.json);
    }
    putend(&w.json);
    putstrkey(&w.json, "finish", finishr);
    if (u) {
        putkey(&w.json, "usage");
        putobject(&w.json);
        putintkey(&w.json, "in", u->in);
        putintkey(&w.json, "out", u->out);
        putintkey(&w.json, "cached", u->cached);
        putend(&w.json);
    }
    putintkey(&w.json, "ms", (os_ticks(s->e->os) - s->sent)/MS);
    ssend(s, &w);

    Msg m = {};
    m.role   = ROLE_ASSISTANT;
    m.blocks = blocks;
    addmsg(s, m);
    if (u && u->in) {
        s->used  = u->in + u->out;
        s->added = 0;
    }
}

// A nudge: an injected user message, counted against its cap. Returns 0
// once the cap is spent, and the caller ends the turn.
static b32 nudge(Session *s, Arena scratch, i32 kind, Str text)
{
    i32 cap = nudgeinfo[kind].cap;
    if (s->nudges[kind] >= cap) {
        return 0;
    }
    i32 n = ++s->nudges[kind];

    Buf t(&scratch, text.len + 64);
    print(&t, text);
    print(&t, " (automatic reminder ");
    print(&t, (i64)n);
    putbyte(&t, '/');
    print(&t, (i64)cap);
    putbyte(&t, ')');
    Str full = finish(&t);

    Buf   b(&scratch, 256 + full.len);
    EvOut w = sbegin(s, &b, EV_NUDGE);
    putstrkey(&w.json, "kind", nudgeinfo[kind].name);
    putintkey(&w.json, "n", n);
    putintkey(&w.json, "cap", cap);
    putstrkey(&w.json, "text", full);
    ssend(s, &w);

    Msg m = {};
    m.role = ROLE_USER;
    m.text = clone(&s->perm, full);
    addmsg(s, m);
    return 1;
}

static void resetnudges(Session *s)
{
    for (i32 i = 0; i < N_COUNT; i++) {
        s->nudges[i] = 0;
    }
}


// Requests

static void stopreq(Session *s)
{
    if (s->http) {
        os_httpcancel(s->e->os, s->http);
        os_httpfree(s->e->os, s->http);
        s->http = 0;
    }
    s->httpid = 0;
    disarm(s, T_FIRSTBYTE);
    disarm(s, T_IDLE);
    disarm(s, T_PROGRESS);
    disarm(s, T_GRACE);
}

static Str authheaders(Arena *a, Session *s, Str accept)
{
    Buf b(a, 256);
    print(&b, "Content-Type: application/json\r\n");
    print(&b, "Accept: ");
    print(&b, accept);
    print(&b, "\r\nUser-Agent: harness/" HARNESS_VERSION "\r\n");
    if (s->ep.key.len) {
        print(&b, "Authorization: Bearer ");
        print(&b, s->ep.key);
        print(&b, "\r\n");
    }
    return finish(&b);
}

// Start a request whose records go to this session. 0 on failure.
static b32 startreq(Session *s, Arena scratch, Str method, Str url, Str body,
                    Str accept, i32 readms)
{
    Engine *e = s->e;
    HttpReq r = {};
    r.method    = method;
    r.url       = url;
    r.headers   = authheaders(&scratch, s, accept);
    r.body      = body;
    r.connectms = s->ep.connectms;
    r.readms    = readms;
    r.maxbytes  = RESPONSE_MAX;
    r.mbox      = e->in;
    r.id        = s->httpid = ++e->nextid;
    s->status   = 0;
    s->headers  = {};
    s->body     = Buf(&s->attempt, 1<<12);
    s->plain    = 0;
    s->sent     = os_ticks(e->os);
    s->http     = os_http(e->os, &r, scratch);
    if (!s->http) {
        s->httpid = 0;
        return 0;
    }
    return 1;
}

static void endturn(Session *, Arena, Str why, Str err);
static void prepare(Session *, Arena);
static void finalize(Session *, Arena);
static void nexttool(Session *, Arena);

static void resetattempt(Session *s)
{
    s->attempt = newarena(s->aregion);
}

static void sendchat(Session *s, Arena scratch)
{
    Engine *e = s->e;
    s->attempts++;
    {
        Arena a = scratch;
        Buf   b(&a, 256);
        EvOut w = sbegin(s, &b, EV_ATTEMPT);
        putintkey(&w.json, "turn", s->turn);
        putintkey(&w.json, "n", s->attempts);
        putstrkey(&w.json, "endpoint", s->ep.url);
        putstrkey(&w.json, "model", s->model);
        ssend(s, &w);
    }

    resetattempt(s);
    s->part    = newpartial(&s->attempt);
    s->sse     = newsse(&s->attempt, SSE_MAX);
    s->cycles  = alloc<Cycle>(&s->attempt, 2);
    s->cycleat = 0;

    // The body goes into the attempt arena, temporaries into scratch
    ChatReq req = {};
    req.mc      = &s->mc;
    req.system  = s->system;
    req.tools   = s->tools;
    req.history = s->history;
    Str body    = encodechat(&s->attempt, scratch, &req);
    Str url     = apiurl(&scratch, s->ep.url, "chat/completions");

    if (!startreq(s, scratch, "POST", url, body, "text/event-stream", 0)) {
        Str err = os_lasterr(e->os, &scratch);
        endturn(s, scratch, "error", concat(&scratch, "cannot start a request: ", err));
        return;
    }
    arm(s, T_FIRSTBYTE, s->ep.firstbyte);
    setstate(s, S_HEAD);
}

static void discover(Session *s, Arena scratch)
{
    Str url = apiurl(&scratch, s->ep.url, "models");
    resetattempt(s);
    s->discovered = 1;
    if (!startreq(s, scratch, "GET", url, {}, "application/json", 10000)) {
        sendchat(s, scratch);
        return;
    }
    arm(s, T_FIRSTBYTE, 15*SEC);
    setstate(s, S_DISCOVER);
}

static void discovered(Session *s, Arena scratch, b32 ok)
{
    Engine *e = s->e;
    if (ok && s->status==200) {
        Str  key     = concat(&scratch, concat(&scratch, s->ep.url, " "), s->model);
        i64 *cached  = upsert(&e->trained, key, (Arena *)0);
        i64  trained = cached ? *cached : 0;
        ModelInfo mi = parsemodels(scratch, finish(&s->body), s->model, trained);
        if (mi.found) {
            s->info        = mi;
            s->mc.llamacpp = mi.llamacpp;
            if (mi.trained && !cached) {
                *upsert(&e->trained, clone(&e->perm, key), &e->perm) = mi.trained;
            }
            if (!s->mc.ctx || mi.learned) {
                // A configured limit holds until a server reports one
                s->mc.ctx = s->mc.ctx && !mi.learned ? s->mc.ctx : mi.ctx;
            }
        }
    }
    stopreq(s);
    sendchat(s, scratch);
}

// Run the complete calls of a committed message.
static void runcalls(Session *s, Arena scratch, Slice<Block> blocks)
{
    s->calls = {};
    for (iz i = 0; i < blocks.len; i++) {
        if (blocks[i].type == BLOCK_CALL) {
            s->calls = push(&s->perm, s->calls, blocks[i]);
        }
    }
    s->callidx = 0;
    nexttool(s, scratch);
}

// Fail the current attempt. Transient failures back off and retry while
// nothing visible has streamed, and otherwise commit what streamed and
// ask the agent to continue. retryms is the server's wish, -1 if none.
static void fail(Session *s, Arena scratch, i32 cls, i32 status, Str msg, i64 retryms)
{
    Engine *e = s->e;
    stopreq(s);
    Partial *p       = s->part;
    b32      visible = p && committed(p);
    s->part = 0;

    if ((cls==CLASS_TRANSIENT || cls==CLASS_LOOP) && visible) {
        // Keep what streamed: the cycle cut away, an incomplete call
        // dropped, complete calls run as usual
        if (cls == CLASS_LOOP) {
            p->text.len = s->cycleat<p->text.len ? s->cycleat : p->text.len;
        }
        Finished f = finishpartial(&s->perm, p, 0, s->turn, s->steps);
        addassistant(s, scratch, f.blocks, "interrupted", 0);
        i32 kind = cls==CLASS_LOOP ? N_LOOP : N_CONTINUE;
        Str text = cls==CLASS_LOOP ?
            Str("Your last response started repeating the same text, so it "
                "was cut off. Stop narrating and repeating yourself: issue "
                "one concrete tool call, or give your final answer.") :
            Str("The previous response was interrupted. Continue from where "
                "you left off without repeating completed content.");
        if (f.ncalls) {
            s->contnext = 1;  // after the calls ran
            s->blocked  = 0;
            runcalls(s, scratch, f.blocks);
            return;
        }
        if (!nudge(s, scratch, kind, text)) {
            Str err = concat(&scratch, "gave up after repeated interruptions: ", msg);
            endturn(s, scratch, "error", err);
            return;
        }
        prepare(s, scratch);
        return;
    }

    if (cls == CLASS_LOOP) {
        // Nothing visible: only reasoning looped, so it is discarded
        Str text = "Your reasoning started repeating itself, so it was cut "
                   "off. Stop deliberating: issue one concrete tool call, or "
                   "give your final answer.";
        if (!nudge(s, scratch, N_LOOP, text)) {
            endturn(s, scratch, "error", "the model kept repeating itself");
            return;
        }
        prepare(s, scratch);
        return;
    }

    if (cls == CLASS_TRANSIENT) {
        if (s->retries >= RETRY_MAX) {
            Str err = concat(&scratch, "gave up after 10 retries: ", msg);
            endturn(s, scratch, "error", err);
            return;
        }
        if (retryms > RETRY_WAIT/MS) {
            Buf b(&scratch, 256 + msg.len);
            print(&b, "the server asked to wait ");
            print(&b, retryms/1000);
            print(&b, " s, longer than the 300 s limit: ");
            print(&b, msg);
            Str err = finish(&b);
            endturn(s, scratch, "error", err);
            return;
        }
        i64 delay = backoffms(++s->retries, os_random(e->os));
        delay = retryms>delay ? retryms : delay;

        Buf   b(&scratch, 256 + msg.len);
        EvOut w = sbegin(s, &b, EV_RETRY);
        putintkey(&w.json, "turn", s->turn);
        putintkey(&w.json, "attempt", s->attempts);
        putstrkey(&w.json, "class", classname(cls));
        putintkey(&w.json, "status", status);
        putintkey(&w.json, "delay_ms", delay);
        putstrkey(&w.json, "msg", msg);
        ssend(s, &w);

        arm(s, T_BACKOFF, delay*MS);
        setstate(s, S_BACKOFF);
        return;
    }

    // Terminal: keep any visible text, but no calls, which would need
    // results that will never come
    if (visible) {
        p->calls = {};
        Finished f = finishpartial(&s->perm, p, 1, s->turn, s->steps);
        if (f.blocks.len) {
            addassistant(s, scratch, f.blocks, "error", 0);
        }
    }
    Buf b(&scratch, 64 + msg.len);
    print(&b, classname(cls));
    print(&b, ": ");
    print(&b, msg);
    Str err = finish(&b);
    endturn(s, scratch, "error", err);
}

static void emitdelta(Session *s, Arena scratch, Delta *d)
{
    Buf   b(&scratch, 256 + d->text.len);
    EvOut w = sbegin(s, &b, EV_DELTA);
    putintkey(&w.json, "turn", s->turn);
    putintkey(&w.json, "attempt", s->attempts);
    putintkey(&w.json, "block", d->block);
    Str kind = d->kind==DELTA_TEXT  ? Str("text") :
               d->kind==DELTA_THINK ? Str("think") : Str("args");
    putstrkey(&w.json, "kind", kind);
    if (d->id.data || d->name.data) {
        putstrkey(&w.json, "id", d->id);
        putstrkey(&w.json, "name", d->name);
    }
    putstrkey(&w.json, "text", d->text);
    ssend(s, &w);
}

static void emitprogress(Session *s, Arena scratch, Chunk *c)
{
    Buf   b(&scratch, 256);
    EvOut w = sbegin(s, &b, EV_PROGRESS);
    putintkey(&w.json, "turn", s->turn);
    putintkey(&w.json, "done", c->pdone);
    putintkey(&w.json, "total", c->ptotal);
    putintkey(&w.json, "cached", c->pcached);
    ssend(s, &w);
}

// Apply one parsed data event. Returns 0 if the attempt failed.
static b32 onchunk(Session *s, Arena scratch, Json *j)
{
    Chunk c = applychunk(s->part, j, &scratch);
    if (c.bad) {
        fail(s, scratch, CLASS_TRANSIENT, 0, "the stream held a malformed chunk", -1);
        return 0;
    } else if (c.error.data) {
        fail(s, scratch, classify(c.errcode, c.error), c.errcode, c.error, -1);
        return 0;
    }
    if (c.prefill) {
        arm(s, T_PROGRESS, s->ep.progress);
        emitprogress(s, scratch, &c);
    }
    if (c.content) {
        arm(s, T_PROGRESS, s->ep.progress);
    }
    for (iz i = 0; i < c.deltas.len; i++) {
        Delta *d = &c.deltas[i];
        emitdelta(s, scratch, d);
        if (d->kind == DELTA_ARGS) {
            continue;
        }
        Cycle *cy  = &s->cycles[d->kind==DELTA_THINK];
        iz     hit = cyclefeed(cy, d->text);
        if (hit >= 0) {
            // Keep the text through the first occurrence of the unit
            s->cycleat = s->part->text.len;
            if (d->kind == DELTA_TEXT) {
                iz end = s->part->text.len - d->text.len + hit;
                s->cycleat = end - (cy->reps-1)*cy->unit;
                s->cycleat = s->cycleat<0 ? 0 : s->cycleat;
            }
            fail(s, scratch, CLASS_LOOP, 0, "the model repeated itself", -1);
            return 0;
        }
    }
    if (s->part->finish.data && !s->timers[T_GRACE]) {
        arm(s, T_GRACE, GRACE);
    }
    return 1;
}

static void onhead(Session *s, i32 status, Str headers)
{
    if (s->state == S_DISCOVER) {
        s->status = status;
        return;
    } else if (s->state != S_HEAD) {
        return;
    }
    disarm(s, T_FIRSTBYTE);
    s->status  = status;
    s->headers = clone(&s->attempt, headers);
    if (status == 200) {
        Str type = headervalue(headers, "content-type");
        s->plain = type.len && !contains(type, "event-stream");
        arm(s, T_IDLE, s->ep.idle);
        arm(s, T_PROGRESS, s->ep.progress);
        setstate(s, S_STREAM);
    } else {
        arm(s, T_IDLE, s->ep.idle);
        setstate(s, S_ERRBODY);
    }
}

static void onbody(Session *s, Arena scratch, Str data)
{
    switch (s->state) {
    case S_DISCOVER:
        if (s->body.len+data.len <= MODELS_MAX) print(&s->body, data);
        return;
    case S_ERRBODY:
        if (s->body.len+data.len <= ERRBODY_MAX) print(&s->body, data);
        arm(s, T_IDLE, s->ep.idle);
        return;
    case S_STREAM:
        break;
    default:
        return;
    }

    arm(s, T_IDLE, s->ep.idle);
    if (s->plain) {
        print(&s->body, data);
        return;
    }
    Slice<SseEvent> evs = ssefeed(&s->sse, data, &scratch);
    for (iz i = 0; i < evs.len; i++) {
        SseEvent *ev = &evs[i];
        if (ev->kind != SSE_EVENT) {
            continue;  // a ping: the idle watchdog was already reset
        }
        if (trim(ev->data) == "[DONE]") {
            s->part->done = 1;
            break;
        }
        Arena   t = scratch;
        JsonDoc d = jsonparse(&t, ev->data);
        if (!d.ok) {
            fail(s, t, CLASS_TRANSIENT, 0, "the stream held a malformed chunk", -1);
            return;
        }
        if (ev->type == "error") {
            Str msg = errortext(jsonget(d.root, "error"));
            fail(s, t, classify(0, msg), 0, msg, -1);
            return;
        }
        if (!onchunk(s, t, d.root)) {
            return;
        }
    }
    if (s->sse.err) {
        fail(s, scratch, CLASS_TRANSIENT, 0, "an event stream line was too long", -1);
    } else if (s->part->done) {
        finalize(s, scratch);
    }
}

static void ondone(Session *s, Arena scratch, i32 err, Str text)
{
    Engine *e = s->e;
    switch (s->state) {
    case S_DISCOVER:
        discovered(s, scratch, err == HTTP_OK);
        return;

    case S_HEAD: {
        Str msg = concat(&scratch, "connection failed: ", text);
        fail(s, scratch, CLASS_TRANSIENT, 0, msg, -1);
    } return;

    case S_ERRBODY: {
        Str body = finish(&s->body);
        Str msg  = errormessage(&scratch, body);
        i32 cls  = classify(s->status, body);
        Buf b(&scratch, 64 + msg.len);
        print(&b, "HTTP ");
        print(&b, (i64)s->status);
        if (msg.len) {
            print(&b, ": ");
            print(&b, msg.len>2000 ? utf8head(msg, 2000) : msg);
        }
        Str full = finish(&b);
        i64 wait = retryafter(s->headers, os_time(e->os));
        fail(s, scratch, cls, s->status, full, wait);
    } return;

    case S_STREAM:
        if (s->plain) {
            JsonDoc d = jsonparse(&scratch, finish(&s->body));
            if (!d.ok) {
                fail(s, scratch, CLASS_TRANSIENT, 0, "the response was not JSON", -1);
                return;
            } else if (!onchunk(s, scratch, d.root)) {
                return;
            }
            s->part->done = 1;
        }
        if (s->part->done || s->part->finish.data) {
            finalize(s, scratch);
        } else if (err == HTTP_OK) {
            fail(s, scratch, CLASS_TRANSIENT, 0,
                 "the stream ended before the response was complete", -1);
        } else {
            Str msg = concat(&scratch, "connection lost: ", text);
            fail(s, scratch, CLASS_TRANSIENT, 0, msg, -1);
        }
        return;
    }
}


// Tools

// Order-independent hash of a value: object members are combined
// commutatively, so key order does not matter.
static u64 jsonhash(Json *j)
{
    u64 h = 0x9e3779b97f4a7c15u ^ (u64)j->type;
    switch (j->type) {
    case JSON_OBJECT: {
        u64 sum = 0;
        for (Json *m = j->child; m; m = m->next) {
            u64 k = hash(m->key) * 0xff51afd7ed558ccdu;
            sum += (k ^ (k>>29)) + jsonhash(m) * 0xc4ceb9fe1a85ec53u;
        }
        return h ^ sum;
    }
    case JSON_ARRAY:
        for (Json *m = j->child; m; m = m->next) {
            h = (h ^ jsonhash(m)) * 1111111111111111111u;
        }
        return h;
    case JSON_STRING:
        return h ^ hash(j->str);
    }
    return h ^ hash(j->raw);
}

// Hash of a step's calls, independent of their order.
static u64 callsethash(Arena scratch, Slice<Block> calls)
{
    u64 sum = 0;
    for (iz i = 0; i < calls.len; i++) {
        Arena  a = scratch;
        Repair r = repairargs(&a, calls[i].args);
        u64    h = hash(calls[i].name) ^ jsonhash(r.root)*0x9e3779b97f4a7c15u;
        sum += h * 1111111111111111111u;
    }
    return sum;
}

static void toolend(Session *s, Arena scratch, Str status, Str text,
                    i32 code, b32 hascode, Str spill, i64 bytes)
{
    Engine *e = s->e;
    Block  *c = &s->calls[s->callidx];
    Buf     b(&scratch, 512 + text.len);
    EvOut   w = sbegin(s, &b, EV_TOOLEND);
    putstrkey(&w.json, "call", c->id);
    putstrkey(&w.json, "status", status);
    if (hascode) {
        putintkey(&w.json, "exit", code);
    }
    putstrkey(&w.json, "text", text);
    if (spill.len) {
        putstrkey(&w.json, "spill", spill);
        putintkey(&w.json, "bytes", bytes);
    }
    putintkey(&w.json, "ms", (os_ticks(e->os) - s->callstart)/MS);
    ssend(s, &w);

    Msg m = {};
    m.role   = ROLE_TOOL;
    m.callid = c->id;
    m.text   = clone(&s->perm, text);
    addmsg(s, m);
    s->progressed |= status == "ok";
    s->callidx++;
}

static void toolstart(Session *s, Arena scratch)
{
    Block *c = &s->calls[s->callidx];
    Buf    b(&scratch, 512 + c->args.len);
    EvOut  w = sbegin(s, &b, EV_TOOLSTART);
    putstrkey(&w.json, "call", c->id);
    putstrkey(&w.json, "name", c->name);
    putstrkey(&w.json, "args", c->args);
    putstrkey(&w.json, "cwd", s->cwd);
    ssend(s, &w);
    s->callstart = os_ticks(s->e->os);
}

// An unattended ask: logged, then denied at once by policy.
static void askdeny(Session *s, Arena scratch, Str kind, Str detail)
{
    Engine *e  = s->e;
    Str     id = s->calls[s->callidx].id;
    Str     rq = concat(&scratch, id, "/ask");
    {
        Buf   b(&scratch, 512 + detail.len);
        EvOut w = sbegin(s, &b, EV_ASK);
        putstrkey(&w.json, "req", rq);
        putstrkey(&w.json, "call", id);
        putstrkey(&w.json, "kind", kind);
        putstrkey(&w.json, "detail", detail);
        putintkey(&w.json, "deadline", os_time(e->os));
        putstrkey(&w.json, "dflt", "deny");
        ssend(s, &w);
    }
    {
        Buf   b(&scratch, 512);
        EvOut w = sbegin(s, &b, EV_ANSWER);
        putstrkey(&w.json, "req", rq);
        putstrkey(&w.json, "decision", "deny");
        putstrkey(&w.json, "by", "policy");
        putstrkey(&w.json, "note", "unattended: nobody can answer");
        ssend(s, &w);
    }
}

static b32 startshell(Session *s, Arena scratch, Args *args)
{
    Engine  *e  = s->e;
    Block   *c  = &s->calls[s->callidx];
    ShellRun sh = {};
    sh.timeout = args->timeout>0 ? args->timeout : SHELL_DEFAULT;
    sh.rel     = clone(&s->perm, concat(&scratch, concat(&scratch, "out/", c->id), ".log"));
    sh.spill   = clone(&s->perm, join(&scratch, s->dir, sh.rel));
    sh.start   = os_ticks(e->os);
    s->sh = sh;

    ShellCmd sc = shellcmd(&scratch, &s->te, args->command);
    Spawn    sp = {};
    sp.argv     = sc.argv;
    sp.cmdline  = sc.cmdline;
    sp.cwd      = s->cwd;
    sp.env      = shellenv(&scratch, e->env, s->te.windows);
    sp.outpath  = s->sh.spill;
    sp.maxbytes = SHELL_OUTMAX;
    sp.mbox     = e->in;
    sp.id       = s->procid = ++e->nextid;
    s->proc = os_spawn(e->os, &sp, scratch);
    if (!s->proc) {
        s->procid = 0;
        return 0;
    }
    arm(s, T_TOOL, s->sh.timeout*SEC);
    setstate(s, S_TOOL);
    return 1;
}

static void finishshell(Session *s, Arena scratch, b32 cancelled)
{
    Engine *e = s->e;
    disarm(s, T_TOOL);
    disarm(s, T_KILL);
    disarm(s, T_DRAIN);
    if (s->proc) {
        os_kill(e->os, s->proc, 1);  // whatever it left behind
        os_procfree(e->os, s->proc);
        s->proc = 0;
    }
    s->procid = 0;
    setstate(s, S_IDLE);

    ShellEnd end  = {};
    end.code      = s->sh.code;
    end.signal    = s->sh.signal;
    end.total     = s->sh.total;
    end.timedout  = s->sh.timedout;
    end.limited   = s->sh.limited;
    end.cut       = s->sh.exited && !s->sh.eof && !cancelled;
    end.cancelled = cancelled;
    end.timeout   = s->sh.timeout;
    ToolOut o = shellresult(e->os, &scratch, s->sh.spill, &end);
    Str status = cancelled    ? Str("cancelled") :
                 end.timedout ? Str("timeout")   :
                 o.ok         ? Str("ok")        : Str("error");
    Stat st = os_stat(e->os, scratch, s->sh.spill);
    i64  n  = st.size>s->sh.total ? st.size : s->sh.total;
    toolend(s, scratch, status, o.text, end.code, !cancelled && !end.timedout,
            s->sh.rel, n);
}

// Run one call. Returns 1 if it finished, 0 if it goes on (a command).
static b32 runtool(Session *s, Arena scratch)
{
    Engine *e = s->e;
    Block  *c = &s->calls[s->callidx];
    toolstart(s, scratch);

    if (s->blocked) {
        Buf b(&scratch, 512);
        print(&b, "This exact set of tool calls was already made 4 times in "
                  "a row, so it was not run again. Its results will not "
                  "change. NEVER call ");
        print(&b, c->name);
        print(&b, " with these arguments again: do something different, or "
                  "give your final answer.");
        Str text = finish(&b);
        toolend(s, scratch, "error", text, 0, 0, {}, 0);
        return 1;
    }

    i32 tool = toolfind(c->name);
    if (tool == TOOL_NONE) {
        Str text = unknowntool(&scratch, c->name);
        toolend(s, scratch, "error", text, 0, 0, {}, 0);
        return 1;
    }
    Args args = parseargs(&scratch, tool, c->args);
    if (args.issues.len) {
        Str text = argserror(&scratch, c->name, c->args, &args);
        if (s->lengthcut && s->callidx==s->calls.len-1) {
            text = concat(&scratch, text, " Your response hit the length "
                          "limit while writing these arguments: write less "
                          "at once, for example a file in several smaller "
                          "edits.");
        }
        toolend(s, scratch, "error", text, 0, 0, {}, 0);
        return 1;
    }

    if (tool != TOOL_SHELL) {
        GuardPath g = guardpath(&scratch, s->cwd, s->outdir, args.path,
                                tool==TOOL_READ, s->te.windows);
        if (!g.inside) {
            Str why = guarddenial(&scratch, s->cwd, g.path, toolnames[tool]);
            askdeny(s, scratch, "path", g.path);
            toolend(s, scratch, "denied", why, 0, 0, {}, 0);
            return 1;
        }
        ToolOut o = {};
        switch (tool) {
        case TOOL_READ:  o = toolread(e->os, &scratch, s->cwd, g.path, &args);  break;
        case TOOL_WRITE: o = toolwrite(e->os, &scratch, s->cwd, g.path, &args); break;
        case TOOL_EDIT:  o = tooledit(e->os, &scratch, s->cwd, g.path, &args);  break;
        }
        toolend(s, scratch, o.ok ? Str("ok") : Str("error"), o.text, 0, 0, {}, 0);
        return 1;
    }

    if (!startshell(s, scratch, &args)) {
        Str err  = os_lasterr(e->os, &scratch);
        Str text = concat(&scratch, "The shell could not be started: ", err);
        toolend(s, scratch, "error", text, 0, 0, {}, 0);
        return 1;
    }
    return 0;
}

static void toolsdone(Session *s, Arena scratch)
{
    s->calls = {};
    setstate(s, S_IDLE);
    if (s->cancelled) {
        endturn(s, scratch, "cancel", {});
        return;
    }
    if (s->progressed) {
        resetnudges(s);
        s->progressed = 0;
    }
    if (s->blocked) {
        s->blocked = 0;
        Str text = "<system-interrupt reason=\"tool_call_loop_detected\">You "
                   "made the same tool calls 5 times in a row and they were "
                   "not run again. Repeating them will not help. Change your "
                   "approach, or give your final answer.</system-interrupt>";
        if (!nudge(s, scratch, N_GUARD, text)) {
            endturn(s, scratch, "error", "the agent kept repeating the same tool calls");
            return;
        }
    }
    if (s->contnext) {
        s->contnext = 0;
        Str text = "The previous response was interrupted. Continue from "
                   "where you left off without repeating completed content.";
        if (!nudge(s, scratch, N_CONTINUE, text)) {
            endturn(s, scratch, "error", "gave up after repeated interruptions");
            return;
        }
    }
    prepare(s, scratch);
}

static void nexttool(Session *s, Arena scratch)
{
    while (s->callidx < s->calls.len) {
        Arena a = scratch;
        if (s->cancelled) {
            toolstart(s, a);
            toolend(s, a, "cancelled", "Not run: the user cancelled the turn.",
                    0, 0, {}, 0);
            continue;
        }
        if (!runtool(s, a)) {
            return;  // a command is running
        }
    }
    toolsdone(s, scratch);
}


// Steps and turns

static void finalize(Session *s, Arena scratch)
{
    Partial *p = s->part;
    stopreq(s);
    s->part = 0;
    Finished f  = finishpartial(&s->perm, p, 1, s->turn, s->steps);
    Str      fr = p->finish.data ? p->finish : Str("stop");
    addassistant(s, scratch, f.blocks, fr, p->hasusage ? &p->usage : 0);
    setstate(s, S_IDLE);

    b32 visible = trim(finish(&p->text)).len > 0;
    if (!f.ncalls) {
        if (fr == "length") {
            Str text = visible ?
                Str("Your response was cut off at the length limit. Continue "
                    "exactly where it stopped.") :
                Str("Your reasoning hit the length limit before you acted. "
                    "Reason only about the next step, then act: write a "
                    "minimal working version now.");
            if (!nudge(s, scratch, visible ? N_CONTINUE : N_LENGTH, text)) {
                endturn(s, scratch, "error", "the response kept hitting the length limit");
                return;
            }
            prepare(s, scratch);
        } else if (!visible) {
            Str text = "Your last response was empty. Continue the task: call "
                       "a tool, or reply with your answer.";
            if (!nudge(s, scratch, N_EMPTY, text)) {
                endturn(s, scratch, "error", "the model kept returning empty responses");
                return;
            }
            prepare(s, scratch);
        } else {
            endturn(s, scratch, "done", {});
        }
        return;
    }

    Slice<Block> calls = {};
    for (iz i = 0; i < f.blocks.len; i++) {
        if (f.blocks[i].type == BLOCK_CALL) {
            calls = push(&scratch, calls, f.blocks[i]);
        }
    }
    u64 set = callsethash(scratch, calls);
    s->setrepeat = set==s->lastset ? s->setrepeat+1 : 1;
    s->lastset   = set;
    s->blocked   = s->setrepeat >= 5;
    s->lengthcut = fr == "length";
    runcalls(s, scratch, f.blocks);
}

// Begin a step: one model response.
static void prepare(Session *s, Arena scratch)
{
    for (iz i = 0; i < s->steers.len; i++) {
        adduser(s, scratch, s->steers[i], "steer");
    }
    s->steers = {};
    if (s->cancelled) {
        endturn(s, scratch, "cancel", {});
        return;
    }

    s->steps++;
    s->retries = 0;
    if (s->steps == STEP_SOFT) {
        note(s, scratch, "warn", "this turn has made 200 model requests; it ends at 300");
    } else if (s->steps > STEP_HARD) {
        endturn(s, scratch, "budget", "the turn reached its limit of 300 model requests");
        return;
    }

    i64 estimate = s->used + s->added/4;
    if (s->mc.ctx && estimate>s->mc.ctx) {
        Buf b(&scratch, 160);
        print(&b, "the conversation (about ");
        print(&b, estimate);
        print(&b, " tokens) no longer fits the model's context of ");
        print(&b, s->mc.ctx);
        print(&b, " tokens");
        Str err = finish(&b);
        endturn(s, scratch, "error", err);
        return;
    }

    if (!s->discovered) {
        discover(s, scratch);
        return;
    }
    sendchat(s, scratch);
}

static void startturn(Session *s, Arena scratch, Str text, Str via)
{
    s->cancelled = 0;
    adduser(s, scratch, text, via);
    s->turn++;
    {
        Arena a = scratch;
        Buf   b(&a, 128);
        EvOut w = sbegin(s, &b, EV_TURN);
        putintkey(&w.json, "n", s->turn);
        ssend(s, &w);
    }
    s->steps      = 0;
    s->attempts   = 0;
    s->setrepeat  = 0;
    s->lastset    = 0;
    s->discovered = s->info.learned || (s->info.found && !s->info.llamacpp);
    resetnudges(s);
    prepare(s, scratch);
}

static void endturn(Session *s, Arena scratch, Str why, Str err)
{
    stopreq(s);
    for (i32 t = 0; t < T_COUNT; t++) {
        disarm(s, t);
    }
    s->part = 0;
    {
        Arena a = scratch;
        Buf   b(&a, 256 + err.len);
        EvOut w = sbegin(s, &b, EV_TURNEND);
        putintkey(&w.json, "turn", s->turn);
        putstrkey(&w.json, "why", why);
        if (err.len) {
            putstrkey(&w.json, "err", err);
        }
        ssend(s, &w);
    }
    setstate(s, S_IDLE);

    // Steering that came too late becomes the next prompt
    for (iz i = 0; i < s->steers.len; i++) {
        s->queued = push(&s->perm, s->queued, s->steers[i]);
    }
    s->steers = {};
    if (s->queued.len && !s->e->quitting) {
        Str next = s->queued[0];
        s->queued.data++;
        s->queued.len--;
        s->queued.cap--;
        startturn(s, scratch, next, "queue");
    }
}

static b32 busy(Session *s)
{
    return s->state!=S_IDLE || s->calls.len;
}

static void cancelturn(Session *s, Arena scratch)
{
    if (!busy(s)) {
        return;
    }
    s->cancelled = 1;
    s->queued    = {};
    switch (s->state) {
    case S_TOOL:
        finishshell(s, scratch, 1);
        nexttool(s, scratch);  // the rest are not run, then the turn ends
        return;
    case S_HEAD:
    case S_STREAM:
    case S_ERRBODY:
        if (s->part && committed(s->part)) {
            s->part->calls = {};
            Finished f = finishpartial(&s->perm, s->part, 1, s->turn, s->steps);
            if (f.blocks.len) {
                addassistant(s, scratch, f.blocks, "cancelled", 0);
            }
        }
        break;
    }
    endturn(s, scratch, "cancel", {});
}


// Sessions

static Session *findsession(Engine *e, Str sid)
{
    for (Session *s = e->sessions; s; s = s->next) {
        if (s->sid == sid) {
            return s;
        }
    }
    return 0;
}

// Time-sortable: YYYYMMDD-HHMMSS-xxxx, in UTC.
static Str newsid(Os *os, Arena *a)
{
    i64 secs = os_time(os) / 1000;
    i64 days = secs / 86400;
    i64 rem  = secs % 86400;
    i64 z    = days + 719468;  // civil from days
    i64 era  = (z >= 0 ? z : z - 146096) / 146097;
    i64 doe  = z - era*146097;
    i64 yoe  = (doe - doe/1460 + doe/36524 - doe/146096) / 365;
    i64 doy  = doe - (365*yoe + yoe/4 - yoe/100);
    i64 mp   = (5*doy + 2)/153;
    i64 d    = doy - (153*mp + 2)/5 + 1;
    i64 m    = mp<10 ? mp+3 : mp-9;
    i64 y    = yoe + era*400 + (m <= 2);

    Buf b(a, 32);
    printpad(&b, y, 4);
    printpad(&b, m, 2);
    printpad(&b, d, 2);
    putbyte(&b, '-');
    printpad(&b, rem/3600, 2);
    printpad(&b, rem/60%60, 2);
    printpad(&b, rem%60, 2);
    putbyte(&b, '-');
    printhex(&b, os_random(os), 4);
    return finish(&b);
}

static i64 seconds(Ini *ini, Str kind, Str name, Str key, i64 dflt)
{
    Str v = iniget(ini, kind, name, key);
    if (v.data) {
        Float f = parsefloat(trim(v));
        if (f.ok && f.val>0 && f.val<1e7) {
            return (i64)(f.val * (f64)SEC);
        }
    }
    return dflt;
}

// Resolve an endpoint by URL or by name. Settings come from the section
// of that name, or the section whose url matches.
static b32 endpointconf(Engine *e, Arena *perm, Str which, Endpoint *ep)
{
    Ini *ini  = e->ini;
    Str  name = {};
    Str  url  = which;
    if (!parseurl(which).ok) {
        name = which;
        url  = iniget(ini, "endpoint", which, "url");
        if (!url.data) {
            return 0;
        }
    } else {
        for (IniSection *s = inifirst(ini, "endpoint"); s; s = ininext(s)) {
            if (iniget(ini, "endpoint", s->name, "url") == url) {
                name = s->name;
            }
        }
    }
    Url u = parseurl(url);
    if (!u.ok) {
        return 0;
    }
    b32 local = localhost(u.host);
    ep->url       = clone(perm, url);
    ep->firstbyte = seconds(ini, "endpoint", name, "first_byte_timeout",
                            local ? 1800*SEC : 300*SEC);
    ep->idle      = seconds(ini, "endpoint", name, "idle_timeout", 90*SEC);
    ep->progress  = seconds(ini, "endpoint", name, "progress_timeout", 600*SEC);
    ep->connectms = (i32)(seconds(ini, "endpoint", name, "connect_timeout",
                                  local ? 5*SEC : 15*SEC) / MS);
    Str key = iniget(ini, "endpoint", name, "key");
    if (startswith(key, "env:")) {
        ep->key = os_getenv(e->os, perm, trim(cuthead(key, 4)));
    } else if (key.len) {
        ep->key = clone(perm, trim(key));  // discouraged, but honored
    }
    return 1;
}

static void modelconf(Engine *e, Arena *perm, Str model, ModelConf *mc)
{
    static Str const sampling[] = {
        "temperature", "top_p", "top_k", "min_p", "presence_penalty",
        "frequency_penalty", "repeat_penalty", "seed", "max_tokens",
    };
    Ini *ini = e->ini;
    mc->name      = clone(perm, model);
    mc->reasoning = "reasoning_content";
    Str r = trim(iniget(ini, "model", model, "reasoning"));
    if (r.data) {
        mc->reasoning = r=="none" || r=="never" ? Str{} : clone(perm, r);
    }
    for (iz i = 0; i < countof(sampling); i++) {
        Str v = trim(iniget(ini, "model", model, sampling[i]));
        if (v.len && parsefloat(v).ok) {
            Extra x = {sampling[i], clone(perm, v)};
            mc->extras = push(perm, mc->extras, x);
        }
    }
    Int ctx = parseint(trim(iniget(ini, "model", model, "context")));
    mc->ctx = ctx.ok && ctx.val>0 ? ctx.val : 0;
}

static Session *newsession(Engine *e, Arena scratch, Str sid)
{
    Os     *os  = e->os;
    b32     big = sizeof(void *) == 8;
    Region *r1  = os_reserve(os, big ? (iz)1<<32 : (iz)1<<26);
    Region *r2  = os_reserve(os, big ? (iz)1<<30 : (iz)1<<26);
    if (!r1 || !r2) {
        return 0;
    }
    Arena    perm = newarena(r1);
    Session *s    = alloc<Session>(&perm);
    s->perm    = perm;
    s->e       = e;
    s->aregion = r2;
    s->attempt = newarena(r2);
    s->sid     = clone(&s->perm, sid);
    Str dir    = join(&scratch, join(&scratch, e->statedir, "sessions"), sid);
    s->dir     = clone(&s->perm, dir);
    s->outdir  = clone(&s->perm, join(&scratch, dir, "out"));
    s->te      = e->te;
    return s;
}

static void sessionready(Session *s)
{
    s->system = systemprompt(&s->perm, s->cwd, &s->te);
    s->tools  = tooljson(&s->perm, &s->te);
    s->next   = s->e->sessions;
    s->e->sessions = s;
}


// Commands

static void reply(Engine *e, Arena scratch, i64 id, b32 ok, Str err, Str sid)
{
    Buf   b(&scratch, 256 + err.len);
    EvOut w = evbegin(&b, sid, 0, os_time(e->os), EV_REPLY);
    putintkey(&w.json, "id", id);
    putintkey(&w.json, "ok", ok);
    if (err.len) {
        putstrkey(&w.json, "err", err);
    }
    emit(e, evend(&w));
}

static void cmdnew(Engine *e, Arena scratch, i64 id, Json *j)
{
    Str cwd   = jsonstr(jsonget(j, "cwd"));
    Str model = jsonstr(jsonget(j, "model"));
    Str which = jsonstr(jsonget(j, "endpoint"));
    if (!which.len) which = iniget(e->ini, "", "", "endpoint");
    if (!model.len) model = iniget(e->ini, "", "", "model");

    Str real = cwd.len ? os_realpath(e->os, &scratch, cwd) : Str{};
    if (!real.len || os_stat(e->os, scratch, real).type!=STAT_DIR) {
        reply(e, scratch, id, 0, "cwd must name an existing directory", {});
        return;
    } else if (!model.len) {
        reply(e, scratch, id, 0, "no model: give one, or set model in the config", {});
        return;
    }
    Endpoint probe = {};
    if (!which.len || !endpointconf(e, &scratch, which, &probe)) {
        reply(e, scratch, id, 0, "no usable endpoint: give a URL or a configured name", {});
        return;
    }

    Str      sid = newsid(e->os, &scratch);
    Session *s   = newsession(e, scratch, sid);
    if (!s) {
        reply(e, scratch, id, 0, "out of memory", {});
        return;
    }
    s->cwd   = clone(&s->perm, real);
    s->model = clone(&s->perm, model);
    endpointconf(e, &s->perm, which, &s->ep);
    modelconf(e, &s->perm, model, &s->mc);

    if (!os_mkdirs(e->os, scratch, s->outdir)) {
        reply(e, scratch, id, 0, "cannot create the session directory", {});
        return;
    }
    Str path = join(&scratch, s->dir, "events.jsonl");
    s->log = logopen(e->os, &s->perm, scratch, path, s->sid, LOG_MAX);
    if (!s->log.ok) {
        reply(e, scratch, id, 0, "cannot open the session log", {});
        return;
    }
    sessionready(s);

    {
        Arena a = scratch;
        Buf   b(&a, 512);
        EvOut w = sbegin(s, &b, EV_SESSION);
        putintkey(&w.json, "proto", 0);
        putstrkey(&w.json, "version", HARNESS_VERSION);
        putstrkey(&w.json, "cwd", s->cwd);
        putstrkey(&w.json, "role", "");
        putstrkey(&w.json, "model", s->model);
        putstrkey(&w.json, "endpoint", s->ep.url);
        ssend(s, &w);
    }
    reply(e, scratch, id, 1, {}, s->sid);

    Str prompt = jsonstr(jsonget(j, "prompt"));
    if (prompt.len) {
        startturn(s, scratch, prompt, "prompt");
    }
}

// Rebuild a session from its log: the history, the turn count, and the
// closing of anything a crash left open (logopen), plus results for
// calls that never started. *firstnew is the seq before any repairs.
static Session *loadsession(Engine *e, Arena scratch, Str sid, Str model,
                            Str which, i64 *firstnew)
{
    Str dir  = join(&scratch, join(&scratch, e->statedir, "sessions"), sid);
    Str path = join(&scratch, dir, "events.jsonl");
    if (os_stat(e->os, scratch, path).type != STAT_FILE) {
        return 0;
    }
    Session *s = newsession(e, scratch, sid);
    if (!s) {
        return 0;
    }
    s->log = logopen(e->os, &s->perm, scratch, path, s->sid, LOG_MAX);
    if (!s->log.ok) {
        return 0;
    }
    *firstnew = s->log.seq - s->log.synth;

    Arena  lines = newarena(e->replay);
    Replay rp    = logreplay(e->os, &lines, scratch, path, s->sid, 0, LOG_MAX);
    Str storedmodel = {};
    Str storedurl   = {};
    for (iz i = 0; i < rp.lines.len; i++) {
        Arena t = scratch;
        EvRec r = evparse(&t, rp.lines[i]);
        Json *j = r.root;
        Msg   m = {};
        switch (r.ev) {
        case EV_SESSION:
            s->cwd      = clone(&s->perm, jsonstr(jsonget(j, "cwd")));
            storedmodel = clone(&s->perm, jsonstr(jsonget(j, "model")));
            storedurl   = clone(&s->perm, jsonstr(jsonget(j, "endpoint")));
            break;
        case EV_TURN:
            s->turn = jsonint(jsonget(j, "n"), s->turn);
            break;
        case EV_USER:
        case EV_NUDGE:
            m.role = ROLE_USER;
            m.text = clone(&s->perm, jsonstr(jsonget(j, "text")));
            addmsg(s, m);
            break;
        case EV_ASSISTANT:
            m.role = ROLE_ASSISTANT;
            for (Json *k = jsonget(j, "blocks")->child; k; k = k->next) {
                Block b = {};
                Str   t = jsonstr(jsonget(k, "t"));
                b.type = t=="think" ? BLOCK_THINK :
                         t=="text"  ? BLOCK_TEXT  : BLOCK_CALL;
                b.text = clone(&s->perm, jsonstr(jsonget(k, "text")));
                b.id   = clone(&s->perm, jsonstr(jsonget(k, "id")));
                b.name = clone(&s->perm, jsonstr(jsonget(k, "name")));
                b.args = clone(&s->perm, jsonstr(jsonget(k, "args")));
                m.blocks = push(&s->perm, m.blocks, b);
            }
            addmsg(s, m);
            break;
        case EV_TOOLEND:
            m.role   = ROLE_TOOL;
            m.callid = clone(&s->perm, jsonstr(jsonget(j, "call")));
            m.text   = clone(&s->perm, jsonstr(jsonget(j, "text")));
            addmsg(s, m);
            break;
        }
    }
    if (!s->cwd.len) {
        return 0;
    }
    s->model = model.len ? clone(&s->perm, model) : storedmodel;
    if (!endpointconf(e, &s->perm, which.len ? which : storedurl, &s->ep)) {
        return 0;
    }
    modelconf(e, &s->perm, s->model, &s->mc);
    sessionready(s);

    // Calls of the last assistant message that never got a result
    iz last = -1;
    for (iz i = 0; i < s->history.len; i++) {
        last = s->history[i].role==ROLE_ASSISTANT ? i : last;
    }
    Slice<Block> blocks = last>=0 ? s->history[last].blocks : Slice<Block>{};
    for (iz i = 0; i < blocks.len; i++) {
        if (blocks[i].type != BLOCK_CALL) continue;
        b32 answered = 0;
        for (iz k = last+1; k < s->history.len; k++) {
            Msg *h = &s->history[k];
            answered |= h->role==ROLE_TOOL && h->callid==blocks[i].id;
        }
        if (!answered) {
            s->calls     = {};
            s->calls     = push(&s->perm, s->calls, blocks[i]);
            s->callidx   = 0;
            s->callstart = os_ticks(e->os);
            toolend(s, scratch, "cancelled", "Not run: the harness stopped "
                    "before this call started.", 0, 0, {}, 0);
        }
    }
    s->calls   = {};
    s->callidx = 0;
    return s;
}

static void cmdopen(Engine *e, Arena scratch, i64 id, Json *j)
{
    Str      sid   = jsonstr(jsonget(j, "sid"));
    i64      from  = jsonint(jsonget(j, "from"), 0);
    Session *s     = findsession(e, sid);
    i64      first = I64MAX;
    if (!s) {
        if (!sid.len || cut(sid, '/').ok || cut(sid, '\\').ok || sid[0]=='.') {
            reply(e, scratch, id, 0, "invalid session id", {});
            return;
        }
        s = loadsession(e, scratch, sid, jsonstr(jsonget(j, "model")),
                        jsonstr(jsonget(j, "endpoint")), &first);
        if (!s) {
            reply(e, scratch, id, 0, "cannot open the session", sid);
            return;
        }
    }
    // from < 0 asks for only what this open added (repairs)
    if (from < 0) {
        from = first==I64MAX ? s->log.seq : first;
    }
    Str    path  = join(&scratch, s->dir, "events.jsonl");
    Arena  lines = newarena(e->replay);
    Replay rp    = logreplay(e->os, &lines, scratch, path, s->sid, from, LOG_MAX);
    for (iz i = 0; i < rp.lines.len; i++) {
        emit(e, rp.lines[i]);
    }
    reply(e, scratch, id, 1, {}, s->sid);
}

static void cmdlist(Engine *e, Arena scratch, i64 id)
{
    struct Entry {
        Str sid;
        Str cwd;
        Str model;
        i64 mtime;
    };
    Slice<Entry> list = {};
    Str dir = join(&scratch, e->statedir, "sessions");
    for (Dirent *d = os_list(e->os, &scratch, dir); d; d = d->next) {
        if (!d->isdir) continue;
        Str   path = join(&scratch, join(&scratch, dir, d->name), "events.jsonl");
        Read  rd   = os_readat(e->os, &scratch, path, 0, 4096);
        Cut   line = cut(rd.data, '\n');
        EvRec r    = evparse(&scratch, line.head);
        if (!line.ok || r.ev!=EV_SESSION) continue;
        Entry en = {};
        en.sid   = d->name;
        en.cwd   = jsonstr(jsonget(r.root, "cwd"));
        en.model = jsonstr(jsonget(r.root, "model"));
        en.mtime = d->mtime;
        list = push(&scratch, list, en);
    }

    Buf   b(&scratch, 4096);
    EvOut w = evbegin(&b, {}, 0, os_time(e->os), EV_SESSIONS);
    putkey(&w.json, "list");
    putarray(&w.json);
    for (iz i = 0; i < list.len; i++) {
        Session *live = findsession(e, list[i].sid);
        putobject(&w.json);
        putstrkey(&w.json, "sid", list[i].sid);
        putstrkey(&w.json, "cwd", list[i].cwd);
        putstrkey(&w.json, "model", list[i].model);
        putstrkey(&w.json, "state", live ? statenames[live->state] : Str("closed"));
        putintkey(&w.json, "mtime", list[i].mtime);
        putend(&w.json);
    }
    putend(&w.json);
    emit(e, evend(&w));
    reply(e, scratch, id, 1, {}, {});
}

static b32 allidle(Engine *e)
{
    for (Session *s = e->sessions; s; s = s->next) {
        if (busy(s)) {
            return 0;
        }
    }
    return 1;
}

static void command(Engine *e, Arena scratch, Str line)
{
    JsonDoc d   = jsonparse(&scratch, line);
    Json   *j   = d.root;
    i64     id  = jsonint(jsonget(j, "id"), 0);
    i32     cmd = cmdfind(jsonstr(jsonget(j, "cmd")));
    if (!d.ok || j->type!=JSON_OBJECT) {
        reply(e, scratch, id, 0, "a command must be a JSON object", {});
        return;
    }

    Str      sid  = jsonstr(jsonget(j, "sid"));
    Str      text = jsonstr(jsonget(j, "text"));
    Session *s    = sid.len ? findsession(e, sid) : 0;
    switch (cmd) {
    case CMD_HELLO: {
        Arena a = scratch;
        Buf   b(&a, 256);
        EvOut w = evbegin(&b, {}, 0, os_time(e->os), EV_HELLO);
        putintkey(&w.json, "proto", 0);
        putstrkey(&w.json, "version", HARNESS_VERSION);
        putkey(&w.json, "caps");
        putarray(&w.json);
        putend(&w.json);
        emit(e, evend(&w));
        reply(e, scratch, id, 1, {}, {});
    } return;
    case CMD_LIST:
        cmdlist(e, scratch, id);
        return;
    case CMD_NEW:
        cmdnew(e, scratch, id, j);
        return;
    case CMD_OPEN:
        cmdopen(e, scratch, id, j);
        return;
    case CMD_QUIT:
        e->quitting = 1;
        for (Session *t = e->sessions; t; t = t->next) {
            cancelturn(t, scratch);
        }
        reply(e, scratch, id, 1, {}, {});
        return;
    case CMD_CONFIGGET:
    case CMD_CONFIGSET:
        reply(e, scratch, id, 0, "not implemented", {});
        return;
    case CMD_NONE:
        reply(e, scratch, id, 0, "unknown command", {});
        return;
    }

    if (!s) {
        reply(e, scratch, id, 0, "no such open session", sid);
        return;
    }
    switch (cmd) {
    case CMD_CLOSE:
        reply(e, scratch, id, 1, {}, sid);
        return;
    case CMD_PROMPT:
    case CMD_STEER:
        if (!text.len) {
            reply(e, scratch, id, 0, "text is required", sid);
            return;
        }
        reply(e, scratch, id, 1, {}, sid);
        if (!busy(s)) {
            startturn(s, scratch, text, cmd==CMD_PROMPT ? Str("prompt") : Str("steer"));
        } else if (cmd == CMD_PROMPT) {
            s->queued = push(&s->perm, s->queued, clone(&s->perm, text));
        } else {
            s->steers = push(&s->perm, s->steers, clone(&s->perm, text));
        }
        return;
    case CMD_CANCEL:
        reply(e, scratch, id, 1, {}, sid);
        cancelturn(s, scratch);
        return;
    case CMD_ANSWER:
        reply(e, scratch, id, 0, "nothing is waiting for an answer", sid);
        return;
    }
}


// The engine thread

static Session *sessionbyid(Engine *e, u64 id, b32 proc)
{
    if (!id) return 0;
    for (Session *s = e->sessions; s; s = s->next) {
        if ((proc ? s->procid : s->httpid) == id) {
            return s;
        }
    }
    return 0;
}

static i32 get32(Str s)
{
    i32 v = 0;
    if (s.len >= 4) __builtin_memcpy(&v, s.data, 4);
    return v;
}

static i64 get64(Str s)
{
    i64 v = 0;
    if (s.len >= 8) __builtin_memcpy(&v, s.data, 8);
    return v;
}

static void onrecord(Engine *e, Arena scratch, Record r)
{
    if (r.type == MAIL_CMD) {
        command(e, scratch, r.data);
        return;
    }

    b32      proc = r.type>=PROC_TAIL && r.type<=PROC_EOF;
    Session *s    = sessionbyid(e, r.id, proc);
    if (!s) {
        return;  // a request or process already given up on
    }
    switch (r.type) {
    case HTTP_HEAD:
        onhead(s, get32(r.data), cuthead(r.data, 4));
        break;
    case HTTP_BODY:
        onbody(s, scratch, r.data);
        break;
    case HTTP_DONE:
        if (s->http) {
            os_httpfree(e->os, s->http);
            s->http = 0;
        }
        ondone(s, scratch, get32(r.data), cuthead(r.data, 4));
        break;
    case PROC_TAIL: {
        Str   tail = cleanoutput(&scratch, utf8tail(cuthead(r.data, 8), 4096));
        Buf   b(&scratch, 256 + tail.len);
        EvOut w = sbegin(s, &b, EV_TOOLOUT);
        putstrkey(&w.json, "call", s->calls[s->callidx].id);
        putstrkey(&w.json, "tail", tail);
        putintkey(&w.json, "bytes", get64(r.data));
        ssend(s, &w);
    } break;
    case PROC_LIMIT:
        s->sh.limited = 1;
        os_kill(e->os, s->proc, 1);
        break;
    case PROC_EXIT:
        s->sh.exited = 1;
        s->sh.code   = get32(r.data);
        s->sh.signal = get32(cuthead(r.data, 4));
        if (s->sh.eof) {
            finishshell(s, scratch, 0);
            nexttool(s, scratch);
        } else {
            arm(s, T_DRAIN, DRAIN);
        }
        break;
    case PROC_EOF:
        s->sh.eof   = 1;
        s->sh.total = get64(r.data);
        if (s->sh.exited) {
            finishshell(s, scratch, 0);
            nexttool(s, scratch);
        }
        break;
    }
}

static void ontimer(Session *s, Arena scratch, i32 t)
{
    Engine *e = s->e;
    Str     msg = {};
    switch (t) {
    case T_FIRSTBYTE:
        if (s->state == S_DISCOVER) {
            discovered(s, scratch, 0);
            return;
        }
        msg = concat(&scratch, "no response within ", secstr(&scratch, s->ep.firstbyte));
        fail(s, scratch, CLASS_TRANSIENT, 0, concat(&scratch, msg, " s"), -1);
        return;
    case T_IDLE:
        msg = concat(&scratch, "no data for ", secstr(&scratch, s->ep.idle));
        fail(s, scratch, CLASS_TRANSIENT, 0, concat(&scratch, msg, " s"), -1);
        return;
    case T_PROGRESS:
        msg = concat(&scratch, "no progress for ", secstr(&scratch, s->ep.progress));
        fail(s, scratch, CLASS_TRANSIENT, 0, concat(&scratch, msg, " s"), -1);
        return;
    case T_GRACE:
        finalize(s, scratch);
        return;
    case T_BACKOFF:
        sendchat(s, scratch);
        return;
    case T_TOOL:
        s->sh.timedout = 1;
        os_kill(e->os, s->proc, 0);
        arm(s, T_KILL, KILLWAIT);
        return;
    case T_KILL:
        os_kill(e->os, s->proc, 1);
        return;
    case T_DRAIN:
        finishshell(s, scratch, 0);
        nexttool(s, scratch);
        return;
    }
}

static void enginethread(void *arg)
{
    Engine *e = (Engine *)arg;
    for (;;) {
        i64 now  = os_ticks(e->os);
        i64 next = I64MAX;
        for (Session *s = e->sessions; s; s = s->next) {
            for (i32 t = 0; t < T_COUNT; t++) {
                if (s->timers[t] && s->timers[t]<next) next = s->timers[t];
            }
        }
        i32 ms = -1;
        if (next != I64MAX) {
            i64 wait = (next - now + MS - 1) / MS;
            ms = wait<0 ? 0 : wait>3600000 ? 3600000 : (i32)wait;
        }
        if (ms) {
            mailwait(e->in, ms);
        }

        Mail m = mailtake(e->in);
        for (Record r; (r = mailnext(&m)).ok;) {
            onrecord(e, newarena(e->scratch), r);
        }

        now = os_ticks(e->os);
        for (Session *s = e->sessions; s; s = s->next) {
            for (i32 t = 0; t < T_COUNT; t++) {
                if (s->timers[t] && s->timers[t]<=now) {
                    s->timers[t] = 0;
                    ontimer(s, newarena(e->scratch), t);
                }
            }
        }

        if (e->quitting && allidle(e)) {
            for (Session *s = e->sessions; s; s = s->next) {
                logclose(&s->log);
            }
            mailpost(e->out, MAIL_QUIT, 0, {}, 1);
            return;
        }
    }
}

// Find the agent's shell: on Windows, busybox-w32 sh next to the
// executable, then on PATH, else cmd.exe; elsewhere /bin/sh.
static ToolEnv findshell(Os *os, Arena *perm, Arena scratch, Str exedir, b32 win)
{
    ToolEnv te = {};
    te.windows = win;
    if (!win) {
        te.shell     = "/bin/sh";
        te.shellkind = SHELL_SH;
        return te;
    }

    Slice<Str> dirs = {};
    dirs = push(&scratch, dirs, exedir);
    Str path = os_getenv(os, &scratch, "PATH");
    for (Str rest = path; rest.len;) {
        Cut c = cut(rest, ';');
        if (c.head.len) dirs = push(&scratch, dirs, c.head);
        rest = c.tail;
    }
    for (iz i = 0; i < dirs.len; i++) {
        Arena a  = scratch;
        Str   sh = join(&a, dirs[i], "sh.exe");
        if (os_stat(os, a, sh).type == STAT_FILE) {
            te.shell     = clone(perm, sh);
            te.shellkind = SHELL_SH;
            return te;
        }
        Str bb = join(&a, dirs[i], "busybox.exe");
        if (os_stat(os, a, bb).type == STAT_FILE) {
            te.shell     = clone(perm, bb);
            te.shellkind = SHELL_BUSYBOX;
            return te;
        }
    }
    Str root = os_getenv(os, &scratch, "SystemRoot");
    Str cmd  = join(&scratch, root.len ? root : Str("C:\\Windows"), "System32\\cmd.exe");
    te.shell     = clone(perm, cmd);
    te.shellkind = SHELL_CMD;
    return te;
}

static Engine *newengine(Os *os, Arena *perm, Ini *ini, Str statedir, Mailbox *out)
{
    b32     big = sizeof(void *) == 8;
    Engine *e   = alloc<Engine>(perm);
    e->os       = os;
    e->out      = out;
    e->ini      = ini;
    e->statedir = clone(perm, statedir);
    e->in       = newmailbox(perm, 4<<20, 0, 0);
    Region *r   = os_reserve(os, big ? (iz)1<<32 : (iz)1<<27);
    e->scratch  = os_reserve(os, big ? (iz)1<<32 : (iz)1<<27);
    e->replay   = os_reserve(os, big ? (iz)1<<32 : (iz)1<<27);
    if (!e->in || !r || !e->scratch || !e->replay) {
        return 0;
    }
    e->perm    = newarena(r);
    Str exe    = os_exepath(os, &e->perm);
    e->exedir  = parentdir(exe);
    e->te      = findshell(os, &e->perm, newarena(e->scratch), e->exedir, HOST_WINDOWS);
    e->env = os_environ(os, &e->perm);
    return e;
}

static b32 enginestart(Engine *e)
{
    return os_thread(e->os, enginethread, e);
}

// Post a command line from any thread.
static void enginecmd(Engine *e, Str line)
{
    mailpost(e->in, MAIL_CMD, 0, line, 1);
}
