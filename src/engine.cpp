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
// Unattended runs must not get stuck, nor give up early, so:
//
// - Context: before each step, a prompt estimated past the model's
//   threshold is compacted (compact.cpp), and so is one the server
//   rejects as too long, retried at 70%, 50%, then 35% of its size. A
//   compaction that leaves the prompt above 80% of the threshold ends
//   the turn rather than compact again and again.
// - Admission: requests wait their turn at their endpoint (admit.cpp),
//   in state "queued", under its limits: requests, different models (a
//   llama.cpp router's max_instances, asked of its /props), and each
//   model's slots.
// - Slow retries and fallbacks: when the quick retries are spent, or a
//   usage limit is hit, the model's fallbacks are tried in turn, each
//   with its own quick retries, and the primary again after a cooldown.
//   Past the end of that chain, slow retries wait minutes ([endpoint]
//   slow_retry) for up to a budget, starting over from the primary.
// - reasoning_end: reasoning that loops, or runs past [model]
//   reasoning_budget, is ended early on llama.cpp by its control
//   endpoint, and elsewhere cut off as a loop.
// - Unexpected stops: a final response that announces work it did not
//   do ("Let me run the tests.") is nudged to continue (provider.cpp).
//
// Asks: a call the guard will not allow by itself (a path outside the
// workspace) needs the user's approval. A session is attended while a
// frontend that can answer has it open, which the frontend declares with
// attended:1 on new or open (and close ends). Unattended, an ask is
// denied at once by policy. Attended, the session logs an ask with a
// deadline ([guard] ask_timeout, 30 s by default) and waits in its own state,
// blocking nothing else: an answer resolves it, the deadline denies it,
// and so does a steer, whose text the agent reads next (as does any ask
// while a steer waits for the next step). Every denial is the call's
// tool result, explaining what to do instead. The log holds ask and
// answer before the call's tool.start, so a crash while asking leaves a
// call that never started rather than an unknown outcome, which the
// next open closes in order: answer, tool.start, tool.end (logopen).
//
// Locks: each session's directory has a lock file, held while a process
// has the session open. A second process opens it read-only: it replays
// and lists, but refuses anything that would write.
//
// This is free and unencumbered software released into the public domain.

enum : u32 {
    MAIL_CMD  = 100,  // frontend to engine: one protocol command line
    MAIL_OUT,         // engine to frontend: one protocol record line
    MAIL_QUIT,        // engine to frontend: the engine has stopped
    MAIL_TOOLONG,     // frontend to engine: a command too long, by its id
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
    QUIT_WAIT    = 5*SEC,         // for turns to end after quit
    LIVE_EVERY   = 1*SEC,         // live records while a request runs
    TITLE_MAX    = 60,            // code points
    SNAPSHOT_MAX = 256 << 10,     // bytes of text in a snapshot
};

enum : i32 {
    S_IDLE,
    S_DISCOVER,  // GET /v1/models
    S_HEAD,      // awaiting the response head
    S_STREAM,    // reading the response
    S_ERRBODY,   // reading an error response
    S_BACKOFF,   // waiting to retry
    S_TOOL,      // a command is running
    S_ASK,       // waiting for the user to answer an ask
    S_QUEUED,    // waiting for its turn at the endpoint
};

static Str statenames[] = {
    "idle", "discover", "waiting", "streaming", "error", "backoff", "tool",
    "ask", "queued",
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
    T_ASK,
    T_LIVE,
    T_ADMIT,     // admitted while queued: send at once
    T_COUNT,
};

enum : i32 {
    N_CONTINUE,  // after an interrupted response
    N_EMPTY,     // nothing visible and no calls
    N_LENGTH,    // stopped at the length limit
    N_LOOP,      // a text cycle
    N_GUARD,     // the same calls 5 times
    N_STOP,      // a final response that announced more work
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
    {"stop",     3},
};
static_assert(countof(nudgeinfo) == N_COUNT);

enum : i32 {
    REQ_CHAT,     // what a request is for: a step of the turn
    REQ_COMPACT,  // a summary of older messages
};

enum : i32 {
    SLOT_NONE,    // a session's place at its endpoint
    SLOT_WAIT,
    SLOT_HELD,
};

enum : i32 {
    CTL_NONE,     // why reasoning was asked to end
    CTL_LOOP,
    CTL_BUDGET,
};

struct Endpoint {
    Str name;       // its configured section, empty if none
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

// An ask waiting for the user, or recently resolved.
struct Ask {
    Str       req;
    i32       tool;
    GuardPath gp;
    GuardHint hint;
    i64       opened;   // ticks
};

// A summary compaction under way (REQ_COMPACT).
struct Compact {
    Endpoint  ep;        // the compactor
    Str       model;
    ModelConf mc;
    i64       goal;      // tokens the prompt must come to, at most
    b32       overflow;  // the server rejected the prompt: no guard
    i64       before;    // tokens, estimated
    iz        start;     // history[start, cut) is summarized
    iz        cut;
    i32       shrink;    // the summarizer rejected its input so often
    iz        inbytes;   // of the conversation in the last request
    iz        rejected;  // of the first one rejected, 0 if none
    i64       began;     // ticks
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
    Slice<Str>   roots;     // extra workspace roots
    Str          model;
    Str          title;
    Endpoint     ep;
    ModelConf    mc;
    ModelInfo    info;
    b32          discovered;  // this turn
    Str          system;
    Str          tools;
    ToolEnv      te;
    Guard        guard;
    EventLog     log;
    iz           lock;       // os_lockfile handle, -1 if not held
    b32          readonly;   // another process holds the lock
    b32          attended;   // a frontend that can answer asks has it open
    Slice<Msg>   history;
    i32          state;
    i64          timers[T_COUNT];
    // Liveness of the current request, in ticks
    i64          lastbyte;   // any byte, or prompt progress
    i64          firstout;   // the first output token
    i64          lastout;
    i64          tokens;     // chunks with output: about one token each
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
    Slice<Str>   queued;     // prompts waiting for the turn to end
    Slice<Str>   steers;     // input for the next step: asks yield to it
    Ask          ask;        // with S_ASK
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
    iz           cycleat;    // where the text to keep ends after a cycle, or -1
    // Tools
    Slice<Block> calls;
    iz           callidx;
    i64          callstart;
    Proc        *proc;
    u64          procid;
    ShellRun     sh;
    // The context: its size, and compaction
    i32          purpose;    // REQ_*: what the current request is for
    i64          bytes;      // of the history, as estimates count them
    Rate         rate;       // tokens to bytes, the server's last count
    i64          sentbytes;  // of the prompt of the chat request sent
    i64          sentest;    // its tokens, estimated
    Str          summary;    // the latest compaction's, empty if none
    Compact      cp;         // with REQ_COMPACT
    i32          overflows;  // this step's prompts rejected as too long
    i64          ovsize;     // tokens of the first, 0 if none
    Overflow     ovinfo;     // the sizes the latest rejection gave
    // Admission at the endpoint
    AdmPoint    *point;      // with a slot
    i32          slot;       // SLOT_*
    Str          slotmodel;
    u64          admid;      // the session's id in queues
    // Fallbacks and slow retries
    Str          home;       // the primary endpoint (name or URL)
    Str          homemodel;  // and model
    i32          target;     // 0 for the primary, else 1 + fallback index
    i64          homeat;     // ticks: the primary may be tried again
    i64          slowfrom;   // ticks: the slow retries began, 0 if none
    i32          slown;      // slow waits so far
    b32          slowing;    // waiting slowly: start over from the primary
    // Ending reasoning early (llama.cpp's reasoning_end)
    i32          ctlwhy;     // CTL_*, for this attempt
    Http        *ctl;
    u64          ctlid;
    i32          ctlstatus;
    Buf          ctlbody;
    b32          noctl;      // the endpoint cannot: cancel instead
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
    Config     *cfg;
    Ini        *ini;      // cfg->ini, until the next reload
    Str         statedir;
    Str         exedir;
    Str         home;     // for "~" in paths
    ToolEnv     te;
    Slice<Str>  env;      // inherited environment
    Session    *sessions;
    Map<i64>   *trained;  // n_ctx_train by "url model", learned when loaded
    Slice<Str>  learned;  // its keys, in order, for saving
    Map<i64>   *kinds;    // by "url model": 1 if llama.cpp's, + slots<<1
    u64         nextid;
    b32         quitting;
    i64         quitby;   // ticks: stop waiting for turns to end
    Adm         adm;      // the queues at each endpoint
    i64         admat;    // ticks: admit again then, 0 if never
    i64         minute;   // ticks of a minute of slow retries (a test
                          // hook, HARNESS_TEST_MINUTE_MS, shortens it)
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

static void note(Session *s, Arena scratch, Str level, Str text)
{
    Buf   b(&scratch, 256 + text.len);
    EvOut w = sbegin(s, &b, EV_NOTE);
    putstrkey(&w.json, "level", level);
    putstrkey(&w.json, "text", text);
    ssend(s, &w);
}

static void arm(Session *s, i32 t, i64 delay)
{
    s->timers[t] = os_ticks(s->e->os) + delay;
}

static void disarm(Session *s, i32 t)
{
    s->timers[t] = 0;
}

// While a request runs, the frontend hears how it goes every second.
static b32 requesting(i32 state)
{
    return state==S_DISCOVER || state==S_HEAD || state==S_STREAM ||
           state==S_ERRBODY;
}

// Output tokens per second, tenths: from the first token to the latest.
static i64 tps10(Session *s)
{
    i64 dt = s->lastout - s->firstout;
    if (s->tokens<2 || dt<=0) {
        return 0;
    }
    return (s->tokens - 1)*10*SEC / dt;
}

// The endpoint of the current request.
static Endpoint *reqep(Session *s)
{
    return s->purpose==REQ_COMPACT ? &s->cp.ep : &s->ep;
}

// The state for the frontend. Small and bounded, so it needs no scratch.
static void live(Session *s)
{
    byte  mem[512];
    Arena a   = {mem, mem+countof(mem)};
    Buf   b(&a, 384);
    Os   *os  = s->e->os;
    i64   now = os_ticks(os);
    EvOut w   = sbegin(s, &b, EV_LIVE);
    b32   cp  = s->purpose==REQ_COMPACT && requesting(s->state);
    putstrkey(&w.json, "state", cp ? Str("compacting") : statenames[s->state]);
    if (requesting(s->state)) {
        Endpoint *ep = reqep(s);
        putintkey(&w.json, "idle_ms", (now - s->lastbyte)/MS);
        putkey(&w.json, "tps");
        putfloat(&w.json, (f64)tps10(s) / 10);
        // The watchdog nearest to firing, and its length, so that a
        // frontend can warn before it fires
        i64 at = 0, of = 0;
        i64 lens[] = {
            s->state==S_DISCOVER ? 15*SEC : ep->firstbyte,
            ep->idle,
            ep->progress,
        };
        for (i32 t = T_FIRSTBYTE; t <= T_PROGRESS; t++) {
            if (s->timers[t] && (!at || s->timers[t]<at)) {
                at = s->timers[t];
                of = lens[t - T_FIRSTBYTE];
            }
        }
        if (at) {
            putintkey(&w.json, "watchdog_ms", (at>now ? at-now : 0)/MS);
            putintkey(&w.json, "watchdog_of", of/MS);
        }
    } else if (s->state == S_BACKOFF) {
        i64 left = s->timers[T_BACKOFF] - now;
        putintkey(&w.json, "retry_at", os_time(os) + left/MS);
    } else if (s->state==S_QUEUED && s->point) {
        putintkey(&w.json, "queue_pos", admpos(s->point, s->admid));
        putintkey(&w.json, "queue_len", admlen(s->point));
    }
    ssend(s, &w);
}

static void setstate(Session *s, i32 state)
{
    if (s->state != state) {
        s->state = state;
        live(s);
        if (requesting(state)) {
            arm(s, T_LIVE, LIVE_EVERY);
        } else {
            disarm(s, T_LIVE);
        }
    }
}


// History

// Append a message, by default of the record just logged.
static void addmsg(Session *s, Msg m)
{
    m.seq = m.seq ? m.seq : s->log.seq;
    s->history = push(&s->perm, s->history, m);
    s->bytes  += msgbytes(&m);
}

// The next prompt's size in tokens, estimated.
static i64 estimate(Session *s)
{
    return tokensof(s->rate, s->system.len + s->tools.len + s->bytes);
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
    if (u && u->in && s->sentbytes) {
        // The server's count calibrates the estimates
        s->rate.tokens = u->in;
        s->rate.bytes  = s->sentbytes;
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


// Worker records

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


// Admission (admit.cpp)

static Session *sessionbyadm(Engine *e, u64 id)
{
    for (Session *s = e->sessions; s; s = s->next) {
        if (s->admid == id) {
            return s;
        }
    }
    return 0;
}

// Admit what may go now at an endpoint: each session admitted sends from
// its T_ADMIT, and those still waiting hear their new places. Then find
// when time alone may admit more, at any endpoint.
static void pump(Engine *e, AdmPoint *p)
{
    i64 now   = os_ticks(e->os);
    b32 moved = 0;
    for (u64 id; (id = admgo(p, &e->perm, now));) {
        Session *s = sessionbyadm(e, id);
        assert(s && s->slot==SLOT_WAIT && s->point==p);
        s->slot = SLOT_HELD;
        arm(s, T_ADMIT, 0);
        moved = 1;
    }
    for (Session *s = e->sessions; moved && s; s = s->next) {
        if (s->point==p && s->slot==SLOT_WAIT && s->state==S_QUEUED) {
            live(s);
        }
    }
    e->admat = 0;
    for (AdmPoint *q = e->adm.points; q; q = q->next) {
        i64 t = admnext(q, now);
        e->admat = t && (!e->admat || t<e->admat) ? t : e->admat;
    }
}

// Give up the session's place at its endpoint: a request ended, with
// linger if the turn goes on, or a wait was abandoned.
static void release(Session *s, b32 linger)
{
    if (s->slot == SLOT_NONE) {
        return;
    }
    Engine *e = s->e;
    if (s->slot == SLOT_HELD) {
        admdone(s->point, s->slotmodel, os_ticks(e->os), linger);
    } else {
        admleave(s->point, s->admid);
    }
    s->slot = SLOT_NONE;
    disarm(s, T_ADMIT);
    pump(e, s->point);
}

static Str authheaders(Arena *a, Endpoint *ep, Str accept)
{
    Buf b(a, 256);
    print(&b, "Content-Type: application/json\r\n");
    print(&b, "Accept: ");
    print(&b, accept);
    print(&b, "\r\nUser-Agent: harness/" HARNESS_VERSION "\r\n");
    if (ep->key.len) {
        print(&b, "Authorization: Bearer ");
        print(&b, ep->key);
        print(&b, "\r\n");
    }
    return finish(&b);
}

// Ask an endpoint about its capacity, once: a llama.cpp router's /props,
// without a model, which loads nothing, gives its max_instances, and a
// single server's gives its total_slots.
static void probe(Engine *e, AdmPoint *p, Endpoint *ep, Arena scratch)
{
    HttpReq r = {};
    r.method    = "GET";
    r.url       = rooturl(&scratch, p->url, "props");
    r.headers   = authheaders(&scratch, ep, "application/json");
    r.connectms = ep->connectms;
    r.readms    = 5000;
    r.maxbytes  = 1 << 20;
    r.mbox      = e->in;
    r.id        = p->probeid = ++e->nextid;
    p->probe    = PROBE_RUNNING;
    p->status   = 0;
    p->body     = Buf(&e->perm, 1<<10);
    p->http     = os_http(e->os, &r, scratch);
    if (!p->http) {
        p->probe   = PROBE_DONE;
        p->probeid = 0;
    }
}

// The limits at an endpoint: as configured, else as learned. Until the
// probe answers, one counted model at a time.
static void pointlimits(AdmPoint *p)
{
    p->maxmodels = p->cfgmodels ? p->cfgmodels :
                   p->probe==PROBE_RUNNING ? 1 : p->models;
}

// A record of an endpoint's probe. Returns 0 if it is not one.
static b32 onprobe(Engine *e, Arena scratch, Record r)
{
    AdmPoint *p = 0;
    for (AdmPoint *q = e->adm.points; r.id && q; q = q->next) {
        p = q->probeid==r.id ? q : p;
    }
    if (!p) {
        return 0;
    }
    switch (r.type) {
    case HTTP_HEAD:
        p->status = get32(r.data);
        break;
    case HTTP_BODY:
        if (p->body.len+r.data.len <= 1<<20) print(&p->body, r.data);
        break;
    case HTTP_DONE: {
        os_httpfree(e->os, p->http);
        p->http    = 0;
        p->probeid = 0;
        p->probe   = PROBE_DONE;
        JsonDoc d  = jsonparse(&scratch, finish(&p->body));
        i64     mi = jsonint(jsonget(d.root, "max_instances"), 0);
        i64     ts = jsonint(jsonget(d.root, "total_slots"), 0);
        b32     rt = jsonstr(jsonget(d.root, "role")) == "router";
        if (p->status == 200) {
            p->models = rt && mi>0 ? (i32)(mi<1024 ? mi : 1024) : 0;
            p->slots  = ts>0 ? (i32)(ts<1024 ? ts : 1024) : 0;
        }
        pointlimits(p);
        pump(e, p);
    } break;
    }
    return 1;
}

static i32 confint(Config *cfg, Str kind, Str name, Str key)
{
    Int v = parseint(trim(cfgget(cfg, kind, name, key, 0)));
    return v.ok && v.val>0 ? (i32)(v.val<1000000 ? v.val : 1000000) : 0;
}

static Str kindkey(Arena *a, Str url, Str model)
{
    return concat(a, concat(a, url, " "), model);
}

// Wait for the current request's turn at its endpoint. Returns 1 if it
// may go now; otherwise the session waits, queued, for its T_ADMIT.
static b32 admit(Session *s, Arena scratch)
{
    Engine   *e     = s->e;
    Config   *cfg   = e->cfg;
    Endpoint *ep    = reqep(s);
    Str       model = s->purpose==REQ_COMPACT ? s->cp.model : s->model;
    AdmPoint *p     = admpoint(&e->adm, &e->perm, ep->url);
    assert(s->slot == SLOT_NONE);

    // What the endpoint's listing says of the model: the session's own
    // from its discovery, any other from the last listing seen
    ModelInfo mi = {};
    if (model==s->model && ep->url==s->ep.url) {
        mi = s->info;
    } else {
        i64 *k = upsert(&e->kinds, kindkey(&scratch, ep->url, model), (Arena *)0);
        mi.llamacpp = k && (*k&1);
        mi.slots    = k ? (i32)(*k>>1) : 0;
    }
    p->maxreq    = confint(cfg, "endpoint", ep->name, "max_concurrent");
    p->cfgmodels = confint(cfg, "endpoint", ep->name, "max_models");
    if (!p->cfgmodels && mi.llamacpp && p->probe==PROBE_NONE) {
        probe(e, p, ep, scratch);
    }
    pointlimits(p);
    i32 cap = confint(cfg, "model", model, "max_concurrent");
    cap = cap ? cap : mi.slots ? mi.slots : p->slots ? p->slots : 4;

    s->admid     = s->admid ? s->admid : ++e->nextid;
    s->point     = p;
    s->slot      = SLOT_WAIT;
    s->slotmodel = model;
    admenqueue(p, &e->perm, s->admid, model, cap, mi.llamacpp || p->cfgmodels,
               os_ticks(e->os));
    pump(e, p);
    if (s->slot == SLOT_HELD) {
        disarm(s, T_ADMIT);  // it goes now
        return 1;
    }
    return 0;
}


// Ending reasoning early: llama.cpp's reasoning_end

static void fail(Session *, Arena, i32 cls, i32 status, Str msg, i64 retryms);

static void stopctl(Session *s)
{
    if (s->ctl) {
        os_httpcancel(s->e->os, s->ctl);
        os_httpfree(s->e->os, s->ctl);
        s->ctl = 0;
    }
    s->ctlid = 0;
}

static ModelConf *reqmc(Session *s)
{
    return s->purpose==REQ_COMPACT ? &s->cp.mc : &s->mc;
}

// Ask llama.cpp to end the response's reasoning now, so that the model
// answers instead, rather than cancel it. Where that cannot be done, or
// was tried already, the attempt fails as a loop. Returns 0 if it did.
static b32 endreasoning(Session *s, Arena scratch, i32 why)
{
    Engine   *e   = s->e;
    Endpoint *ep  = reqep(s);
    Str       msg = why==CTL_BUDGET ? Str("the reasoning ran past its budget")
                                    : Str("the model's reasoning repeated itself");
    b32       can = reqmc(s)->llamacpp && !s->noctl && s->part->id.len;
    if (s->ctlwhy || !can) {
        s->ctlwhy = why;
        fail(s, scratch, CLASS_LOOP, 0, msg, -1);
        return 0;
    }

    Buf     b(&scratch, 256);
    JsonOut w = {&b};
    putobject(&w);
    putstrkey(&w, "id", s->part->id);
    putstrkey(&w, "action", "reasoning_end");
    putstrkey(&w, "model", reqmc(s)->name);
    putend(&w);
    Str body = finish(&b);

    HttpReq r = {};
    r.method    = "POST";
    r.url       = apiurl(&scratch, ep->url, "chat/completions/control");
    r.headers   = authheaders(&scratch, ep, "application/json");
    r.body      = body;
    r.connectms = ep->connectms;
    r.readms    = 10000;
    r.maxbytes  = 64 << 10;
    r.mbox      = e->in;
    r.id        = s->ctlid = ++e->nextid;
    s->ctlwhy    = why;
    s->ctlstatus = 0;
    s->ctlbody   = Buf(&s->attempt, 256);
    s->ctl       = os_http(e->os, &r, scratch);
    if (!s->ctl) {
        s->ctlid = 0;
        fail(s, scratch, CLASS_LOOP, 0, msg, -1);
        return 0;
    }
    Str text = concat(&scratch, msg, ": asked the server to end it (reasoning_end)");
    note(s, scratch, "info", text);
    return 1;
}

// A record of a reasoning_end request. Returns 0 if it is not one.
static b32 onctl(Engine *e, Arena scratch, Record r)
{
    Session *s = 0;
    for (Session *t = e->sessions; r.id && t; t = t->next) {
        s = t->ctlid==r.id ? t : s;
    }
    if (!s) {
        return 0;
    }
    switch (r.type) {
    case HTTP_HEAD:
        s->ctlstatus = get32(r.data);
        break;
    case HTTP_BODY:
        if (s->ctlbody.len+r.data.len <= 4<<10) print(&s->ctlbody, r.data);
        break;
    case HTTP_DONE: {
        stopctl(s);
        Str     body = finish(&s->ctlbody);
        JsonDoc d    = jsonparse(&scratch, body);
        Json   *ok   = jsonget(d.root, "success");
        if (s->ctlstatus==200 && ok->type==JSON_BOOL && ok->boolean) {
            break;  // the model answers next
        } else if (s->ctlstatus==200 && contains(body, "no active completion")) {
            break;  // it ended meanwhile
        }
        // Not supported here: cancel this time, and from now on
        s->noctl = 1;
        if (s->state==S_STREAM && s->part) {
            Str msg = s->ctlwhy==CTL_BUDGET ?
                Str("the reasoning ran past its budget, and the server cannot end it") :
                Str("the model's reasoning repeated itself, and the server cannot end it");
            fail(s, scratch, CLASS_LOOP, 0, msg, -1);
        }
    } break;
    }
    return 1;
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
    stopctl(s);
    release(s, 1);
    disarm(s, T_FIRSTBYTE);
    disarm(s, T_IDLE);
    disarm(s, T_PROGRESS);
    disarm(s, T_GRACE);
}

// Start a request whose records go to this session. 0 on failure.
static b32 startreq(Session *s, Arena scratch, Endpoint *ep, Str method,
                    Str url, Str body, Str accept, i32 readms)
{
    Engine *e = s->e;
    HttpReq r = {};
    r.method    = method;
    r.url       = url;
    r.headers   = authheaders(&scratch, ep, accept);
    r.body      = body;
    r.connectms = ep->connectms;
    r.readms    = readms;
    r.maxbytes  = RESPONSE_MAX;
    r.mbox      = e->in;
    r.id        = s->httpid = ++e->nextid;
    s->status   = 0;
    s->headers  = {};
    s->body     = Buf(&s->attempt, 1<<12);
    s->plain    = 0;
    s->sent     = os_ticks(e->os);
    s->lastbyte = s->sent;
    s->firstout = 0;
    s->lastout  = 0;
    s->tokens   = 0;
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
static void sendcompact(Session *, Arena);
static void compact(Session *, Arena, i64 enough, i64 goal, b32 overflow);

static void resetattempt(Session *s)
{
    s->attempt = newarena(s->aregion);
}

// A new attempt's response state, in its fresh arena.
static void newattempt(Session *s)
{
    resetattempt(s);
    s->part    = newpartial(&s->attempt);
    s->sse     = newsse(&s->attempt, SSE_MAX);
    s->cycles  = alloc<Cycle>(&s->attempt, 2);
    s->cycleat = -1;
    s->ctlwhy  = CTL_NONE;
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

    newattempt(s);

    // The body goes into the attempt arena, temporaries into scratch
    ChatReq req = {};
    req.mc       = &s->mc;
    req.system   = s->system;
    req.tools    = s->tools;
    req.history  = s->history;
    Str body     = encodechat(&s->attempt, scratch, &req);
    Str url      = apiurl(&scratch, s->ep.url, "chat/completions");
    s->sentbytes = s->system.len + s->tools.len + s->bytes;
    s->sentest   = estimate(s);

    if (!startreq(s, scratch, &s->ep, "POST", url, body, "text/event-stream", 0)) {
        Str err = os_lasterr(e->os, &scratch);
        err = concat(&scratch, "cannot start a request: ", err);
        endturn(s, scratch, "error", err);
        return;
    }
    arm(s, T_FIRSTBYTE, s->ep.firstbyte);
    setstate(s, S_HEAD);
}

static void sendreq(Session *s, Arena scratch)
{
    if (s->purpose == REQ_COMPACT) {
        sendcompact(s, scratch);
    } else {
        sendchat(s, scratch);
    }
}

// Send the current request once it is admitted at its endpoint.
static void request(Session *s, Arena scratch)
{
    if (!admit(s, scratch)) {
        setstate(s, S_QUEUED);
        return;
    }
    sendreq(s, scratch);
}

static void discover(Session *, Arena);

// Carry on toward the step's request: the model's listing first, then
// room in the context, then its turn at the endpoint.
static void proceed(Session *s, Arena scratch)
{
    if (!s->discovered) {
        discover(s, scratch);
        return;
    }
    if (s->purpose==REQ_CHAT && s->mc.ctx) {
        i64 at = compactat(s->mc.ctx);
        if (estimate(s) >= at) {
            compact(s, scratch, at, at*4/5, 0);
            return;
        }
    }
    request(s, scratch);
}

static void discover(Session *s, Arena scratch)
{
    Str url = apiurl(&scratch, s->ep.url, "models");
    resetattempt(s);
    s->discovered = 1;
    if (!startreq(s, scratch, &s->ep, "GET", url, {}, "application/json", 10000)) {
        proceed(s, scratch);
        return;
    }
    arm(s, T_FIRSTBYTE, 15*SEC);
    setstate(s, S_DISCOVER);
}

// Trained context sizes survive restarts in state/models.json, so that a
// model seen loaded once is judged right while it is unloaded.
static Str modelspath(Arena *a, Engine *e)
{
    return join(a, e->statedir, "models.json");
}

static void loadtrained(Engine *e, Arena scratch)
{
    Read    rd = os_read(e->os, &scratch, modelspath(&scratch, e), 1<<20);
    JsonDoc d  = jsonparse(&scratch, rd.data);
    for (Json *m = d.root->child; d.root->type==JSON_OBJECT && m; m = m->next) {
        i64 v = jsonint(m, 0);
        if (v > 0) {
            Str key = clone(&e->perm, m->key);
            *upsert(&e->trained, key, &e->perm) = v;
            e->learned = push(&e->perm, e->learned, key);
        }
    }
}

static void savetrained(Engine *e, Arena scratch)
{
    Buf     b(&scratch, 1024);
    JsonOut w = {&b};
    putobject(&w);
    for (iz i = 0; i < e->learned.len; i++) {
        putkey(&w, e->learned[i]);
        putint(&w, *upsert(&e->trained, e->learned[i], (Arena *)0));
    }
    putend(&w);
    putbyte(&b, '\n');
    Str data = finish(&b);
    Str path = modelspath(&scratch, e);
    os_write(e->os, scratch, path, data);
}

// Note what a listing says of every model, for requests to models other
// than a session's own (a compactor).
static void notekinds(Engine *e, Arena scratch, Str url, Str body)
{
    JsonDoc d = jsonparse(&scratch, body);
    for (Json *m = jsonget(d.root, "data")->child; m; m = m->next) {
        Str id = jsonstr(jsonget(m, "id"));
        if (id.len) {
            ModelInfo mi = modelinfo(m, 0);
            Arena     t  = scratch;
            Str       k  = kindkey(&t, url, id);
            i64      *v  = upsert(&e->kinds, k, (Arena *)0);
            v = v ? v : upsert(&e->kinds, clone(&e->perm, k), &e->perm);
            *v = (i64)mi.llamacpp | (i64)mi.slots<<1;
        }
    }
}

static void discovered(Session *s, Arena scratch, b32 ok)
{
    Engine *e = s->e;
    if (ok && s->status==200) {
        notekinds(e, scratch, s->ep.url, finish(&s->body));
        Str  key     = concat(&scratch, concat(&scratch, s->ep.url, " "), s->model);
        i64 *cached  = upsert(&e->trained, key, (Arena *)0);
        i64  trained = cached ? *cached : 0;
        ModelInfo mi = parsemodels(scratch, finish(&s->body), s->model, trained);
        if (mi.found) {
            s->info        = mi;
            s->mc.llamacpp = mi.llamacpp;
            if (mi.trained && mi.trained!=trained) {
                if (!cached) {
                    key    = clone(&e->perm, key);
                    cached = upsert(&e->trained, key, &e->perm);
                    e->learned = push(&e->perm, e->learned, key);
                }
                *cached = mi.trained;
                savetrained(e, scratch);
            }
            if (!s->mc.ctx || mi.learned) {
                // A configured limit holds until a server reports one
                s->mc.ctx = s->mc.ctx && !mi.learned ? s->mc.ctx : mi.ctx;
            }
        }
    }
    stopreq(s);
    proceed(s, scratch);
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

// Retries and fallbacks

static b32  endpointconf(Engine *, Arena *, Str which, Endpoint *);
static void modelconf(Engine *, Arena *, Str model, ModelConf *);

// Ticks of slow waiting and cooldowns, as the test hook has them.
static i64 scaled(Engine *e, i64 ticks)
{
    return (i64)((f64)ticks * (f64)e->minute / (f64)(60*SEC));
}

enum : i32 {
    RETRY_QUICK,   // backing off a little
    RETRY_SLOW,    // minutes
    RETRY_SWITCH,  // to a fallback, at once
};

// A retry record: its target (endpoint and model) is the next attempt's.
static void retryevent(Session *s, Arena scratch, i32 kind, i32 cls,
                       i32 status, i64 delayms, Str msg)
{
    Str   model = s->purpose==REQ_COMPACT ? s->cp.model : s->model;
    Buf   b(&scratch, 512 + msg.len);
    EvOut w = sbegin(s, &b, EV_RETRY);
    putintkey(&w.json, "turn", s->turn);
    putintkey(&w.json, "attempt", s->attempts);
    putstrkey(&w.json, "class", classname(cls));
    putintkey(&w.json, "status", status);
    putintkey(&w.json, "delay_ms", delayms);
    putstrkey(&w.json, "msg", msg);
    putstrkey(&w.json, "endpoint", reqep(s)->url);
    putstrkey(&w.json, "model", model);
    if (kind == RETRY_SLOW) {
        putintkey(&w.json, "slow", 1);
    } else if (kind == RETRY_SWITCH) {
        putintkey(&w.json, "fallback", s->target);
    }
    if (s->purpose == REQ_COMPACT) {
        putintkey(&w.json, "compact", 1);
    }
    ssend(s, &w);
}

// Back off and retry, while quick retries are left. Returns 0 once they
// are spent. retryms is the server's wish, -1 if none.
static b32 quickretry(Session *s, Arena scratch, i32 cls, i32 status, Str msg,
                      i64 retryms)
{
    Engine *e = s->e;
    if (s->retries >= RETRY_MAX) {
        return 0;
    }
    i64 delay = scaled(e, backoffms(++s->retries, os_random(e->os)));
    delay = retryms>delay ? retryms : delay;
    retryevent(s, scratch, RETRY_QUICK, cls, status, delay, msg);
    arm(s, T_BACKOFF, delay*MS);
    setstate(s, S_BACKOFF);
    return 1;
}

// Once quick retries are spent and no fallback is left, wait minutes at
// a time, as [endpoint] slow_retry says (the last wait repeating), then
// start over from the primary, until [endpoint] slow_retry_budget is
// spent; a request that succeeds resets it. The server's own wish is
// honored if longer. Returns 0 past the budget.
static b32 slowwait(Session *s, Arena scratch, i32 cls, i32 status, Str msg,
                    i64 retryms)
{
    Engine *e    = s->e;
    Str     name = reqep(s)->name;
    i64     now  = os_ticks(e->os);
    i64     wait = 60*SEC;
    i32     n    = 0;
    Str     list = cfgget(e->cfg, "endpoint", name, "slow_retry", 0);
    for (Str rest = list; rest.len;) {
        Float f = parsefloat(cfgnextword(&rest));
        if (f.ok && f.val>0) {
            wait = (i64)(f.val * (f64)SEC);
            if (n++ == s->slown) break;
        }
    }
    wait = scaled(e, wait);
    wait = retryms*MS>wait ? retryms*MS : wait;  // the server's clock
    i64 budget = scaled(e, cfgms(e->cfg, "endpoint", name, "slow_retry_budget", 0)*MS);
    s->slowfrom = s->slowfrom ? s->slowfrom : now;
    if (now+wait-s->slowfrom > budget) {
        return 0;
    }
    s->slown++;
    s->retries = 0;  // quick ones again after the wait
    s->slowing = 1;
    retryevent(s, scratch, RETRY_SLOW, cls, status, wait/MS, msg);
    arm(s, T_BACKOFF, wait);
    setstate(s, S_BACKOFF);
    return 1;
}

// Make an endpoint (name or URL) and model the target of the session's
// next requests, whose listing is read again first.
static b32 usetarget(Session *s, Str which, Str model)
{
    Engine  *e  = s->e;
    Endpoint ep = {};
    if (!endpointconf(e, &s->perm, which, &ep)) {
        return 0;
    }
    ModelConf mc = {};
    s->ep    = ep;
    s->model = clone(&s->perm, model);
    modelconf(e, &s->perm, s->model, &mc);
    s->mc         = mc;
    s->info       = {};
    s->discovered = 0;
    s->retries    = 0;
    return 1;
}

// A fallback is MODEL, at the primary's endpoint, or ENDPOINT/MODEL with
// ENDPOINT a configured name.
static b32 usefallback(Session *s, Str spec)
{
    Cut c = cut(spec, '/');
    if (c.ok && c.tail.len &&
            trim(cfgget(s->e->cfg, "endpoint", c.head, "url", 0)).len) {
        return usetarget(s, c.head, c.tail);
    }
    return usetarget(s, s->home, spec);
}

// Move on to the next of the primary's fallbacks ([model] fallback), if
// one is left, which gets quick retries of its own. Leaving the primary
// starts its cooldown. Returns 0 at the end of the chain.
static b32 nexttarget(Session *s, Arena scratch, i32 cls, i32 status, Str msg)
{
    Engine    *e     = s->e;
    Str        list  = cfgget(e->cfg, "model", s->homemodel, "fallback", 0);
    Slice<Str> chain = {};
    for (Str rest = list; rest.len;) {
        Str w = cfgnextword(&rest);
        if (w.len) chain = push(&scratch, chain, w);
    }
    for (iz i = s->target; i < chain.len; i++) {
        if (!usefallback(s, chain[i])) {
            continue;  // names nothing usable
        }
        if (!s->target) {
            i64 cool  = cfgms(e->cfg, "model", s->homemodel, "fallback_cooldown", 0);
            s->homeat = os_ticks(e->os) + scaled(e, cool*MS);
        }
        s->target = (i32)i + 1;
        retryevent(s, scratch, RETRY_SWITCH, cls, status, 0, msg);
        return 1;
    }
    return 0;
}

// Back to the primary model from a fallback.
static void gohome(Session *s, Arena scratch, Str why)
{
    if (!s->target) {
        return;
    }
    Str from = s->model;
    if (!usetarget(s, s->home, s->homemodel)) {
        return;  // its endpoint left the configuration
    }
    s->target = 0;
    Buf b(&scratch, 256 + from.len + why.len);
    print(&b, "trying the primary model, ");
    print(&b, s->model);
    print(&b, ", again after ");
    print(&b, from);
    print(&b, ": ");
    print(&b, why);
    Str text = finish(&b);
    note(s, scratch, "info", text);
}

// The server rejected the prompt as too long: compact to 70% of its
// size, then 50%, then 35%, retrying the step after each, then give up.
// The server's numbers correct the estimates and the window.
static void overflowed(Session *s, Arena scratch, Str msg)
{
    static i32 const pct[] = {70, 50, 35};
    Overflow o = s->ovinfo;
    s->ovinfo = {};
    if (o.ctx>0 && (!s->mc.ctx || o.ctx<s->mc.ctx)) {
        s->mc.ctx = o.ctx;
    }
    if (o.prompt>0 && s->sentbytes>0) {
        s->rate.tokens = o.prompt;
        s->rate.bytes  = s->sentbytes;
    }
    i64 size  = o.prompt>0 ? o.prompt : s->sentest;
    s->ovsize = s->ovsize ? s->ovsize : size;
    if (s->overflows >= countof(pct)) {
        Str err = concat(&scratch, "overflow: still too long after compacting "
                         "3 times: ", msg);
        endturn(s, scratch, "error", err);
        return;
    }
    i64 goal = s->ovsize * pct[s->overflows++] / 100;
    if (s->mc.ctx) {
        i64 most = compactat(s->mc.ctx) * 4/5;
        goal = goal<most ? goal : most;
    }
    compact(s, scratch, goal, goal, 1);
}

// A summary request failed: transient failures retry as any request
// does, quickly then slowly; a summarizer that finds its input too long
// gets the newest 70%, then 50%, then 35% of it; the rest end the turn.
static void failcompact(Session *s, Arena scratch, i32 cls, i32 status,
                        Str msg, i64 retryms)
{
    stopreq(s);
    s->part = 0;
    if (cls==CLASS_OVERFLOW && s->cp.shrink<3) {
        s->cp.rejected = s->cp.rejected ? s->cp.rejected : s->cp.inbytes;
        s->cp.shrink++;
        retryevent(s, scratch, RETRY_QUICK, cls, status, 0, msg);
        request(s, scratch);
        return;
    }
    b32 again = cls==CLASS_TRANSIENT || cls==CLASS_LOOP;
    b32 later = retryms > RETRY_WAIT/MS;
    if (again && !later && quickretry(s, scratch, cls, status, msg, retryms)) {
        return;
    } else if (again && slowwait(s, scratch, cls, status, msg, later ? retryms : -1)) {
        return;
    }
    Buf b(&scratch, 64 + msg.len);
    print(&b, "compaction failed: ");
    print(&b, classname(cls));
    print(&b, ": ");
    print(&b, msg);
    Str err = finish(&b);
    endturn(s, scratch, "error", err);
}

// Fail the current attempt. Transient failures back off and retry while
// nothing visible has streamed, and otherwise commit what streamed and
// ask the agent to continue. Once quick retries are spent, fallbacks and
// slow retries follow (above). retryms is the server's wish, -1 if none.
static void fail(Session *s, Arena scratch, i32 cls, i32 status, Str msg, i64 retryms)
{
    if (s->purpose == REQ_COMPACT) {
        failcompact(s, scratch, cls, status, msg, retryms);
        return;
    }
    stopreq(s);
    Partial *p       = s->part;
    b32      visible = p && committed(p);
    s->part = 0;

    if ((cls==CLASS_TRANSIENT || cls==CLASS_LOOP) && visible) {
        // Keep what streamed: the cycle cut away, an incomplete call
        // dropped, complete calls run as usual
        if (cls==CLASS_LOOP && s->cycleat>=0) {
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
        Str text = s->ctlwhy==CTL_BUDGET ?
            Str("Your reasoning ran past its budget, so it was cut off. Stop "
                "deliberating: issue one concrete tool call, or give your "
                "final answer.") :
            Str("Your reasoning started repeating itself, so it was cut "
                "off. Stop deliberating: issue one concrete tool call, or "
                "give your final answer.");
        if (!nudge(s, scratch, N_LOOP, text)) {
            endturn(s, scratch, "error", "the model kept repeating itself");
            return;
        }
        prepare(s, scratch);
        return;
    } else if (cls == CLASS_OVERFLOW) {
        overflowed(s, scratch, msg);
        return;
    }

    // A wait longer than quick retries honor is a usage limit, as is a
    // spending or quota error. A missing model (404) is unavailable.
    b32 later = cls==CLASS_TRANSIENT && retryms>RETRY_WAIT/MS;
    b32 gone  = cls==CLASS_DETERMINISTIC && status==404;
    if (cls==CLASS_TRANSIENT && !later &&
            quickretry(s, scratch, cls, status, msg, retryms)) {
        return;
    } else if ((cls==CLASS_TRANSIENT || cls==CLASS_LIMIT || gone) &&
               nexttarget(s, scratch, cls, status, msg)) {
        proceed(s, scratch);
        return;
    } else if (cls==CLASS_TRANSIENT &&
               slowwait(s, scratch, cls, status, msg, later ? retryms : -1)) {
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
    Buf b(&scratch, 256 + msg.len);
    if (later) {
        print(&b, "the server asked to wait ");
        print(&b, retryms/1000);
        print(&b, " s, longer than slow retries may: ");
    } else if (cls == CLASS_TRANSIENT) {
        print(&b, "gave up after ");
        print(&b, (i64)s->slown);
        print(&b, " slow retries over ");
        print(&b, (os_ticks(s->e->os) - s->slowfrom)/SEC);
        print(&b, " s: ");
    } else {
        print(&b, classname(cls));
        print(&b, ": ");
    }
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
    Endpoint *ep = reqep(s);
    if (c.prefill) {
        arm(s, T_PROGRESS, ep->progress);
        emitprogress(s, scratch, &c);
    }
    if (c.content) {
        arm(s, T_PROGRESS, ep->progress);
        s->lastout  = os_ticks(s->e->os);
        s->firstout = s->firstout ? s->firstout : s->lastout;
        s->tokens++;
    }
    for (iz i = 0; i < c.deltas.len; i++) {
        Delta *d = &c.deltas[i];
        if (s->purpose == REQ_CHAT) {
            emitdelta(s, scratch, d);  // a summary is not the agent's
        }
        if (d->kind == DELTA_ARGS) {
            continue;
        }
        Cycle *cy  = &s->cycles[d->kind==DELTA_THINK];
        iz     hit = cyclefeed(cy, d->text);
        if (hit>=0 && d->kind==DELTA_THINK) {
            // Looping reasoning is ended early if possible, and else it
            // goes, while the text is kept through the first occurrence
            // of its unit
            *cy = {};
            if (!endreasoning(s, scratch, CTL_LOOP)) {
                return 0;
            }
        } else if (hit >= 0) {
            iz end = s->part->text.len - d->text.len + hit;
            s->cycleat = end - (cy->reps-1)*cy->unit;
            s->cycleat = s->cycleat<0 ? 0 : s->cycleat;
            fail(s, scratch, CLASS_LOOP, 0, "the model repeated itself", -1);
            return 0;
        }
    }

    // Reasoning past its budget is ended likewise, and cut off should
    // it run on to twice the budget regardless
    i64 most = reqmc(s)->thinkmax;
    i64 used = tokensof(s->rate, s->part->think.len);
    if (most && used>most && (!s->ctlwhy || used>2*most) &&
            !endreasoning(s, scratch, CTL_BUDGET)) {
        return 0;
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
    s->lastbyte = os_ticks(s->e->os);
    s->status   = status;
    s->headers  = clone(&s->attempt, headers);
    if (status == 200) {
        Str type = headervalue(headers, "content-type");
        s->plain = type.len && !contains(type, "event-stream");
        arm(s, T_IDLE, reqep(s)->idle);
        arm(s, T_PROGRESS, reqep(s)->progress);
        setstate(s, S_STREAM);
    } else {
        arm(s, T_IDLE, reqep(s)->idle);
        setstate(s, S_ERRBODY);
    }
}

static void onbody(Session *s, Arena scratch, Str data)
{
    s->lastbyte = os_ticks(s->e->os);
    switch (s->state) {
    case S_DISCOVER:
        if (s->body.len+data.len <= MODELS_MAX) print(&s->body, data);
        return;
    case S_ERRBODY:
        if (s->body.len+data.len <= ERRBODY_MAX) print(&s->body, data);
        arm(s, T_IDLE, reqep(s)->idle);
        return;
    case S_STREAM:
        break;
    default:
        return;
    }

    arm(s, T_IDLE, reqep(s)->idle);
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
        if (cls == CLASS_OVERFLOW) {
            s->ovinfo = overflowsizes(scratch, body);
        }
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
    m.spill  = spill.len ? clone(&s->perm, spill) : Str{};
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

// A file tool on a path the guard allowed, or the user did.
static void runfile(Session *s, Arena scratch, i32 tool, Str path, Args *args)
{
    Engine *e    = s->e;
    ToolOut o    = {};
    Str     ncwd = s->guard.cwd;  // as normalized paths
    switch (tool) {
    case TOOL_READ:  o = toolread(e->os, &scratch, ncwd, path, args);  break;
    case TOOL_WRITE: o = toolwrite(e->os, &scratch, ncwd, path, args); break;
    case TOOL_EDIT:  o = tooledit(e->os, &scratch, ncwd, path, args);  break;
    }
    toolend(s, scratch, o.ok ? Str("ok") : Str("error"), o.text, 0, 0, {}, 0);
}

static Str bynames[] = {"policy", "user", "timeout", "steer", "cancel"};
static_assert(countof(bynames) == DENY_COUNT);

// Resolve the current call's ask: log the answer, then run the call, or
// end it with the denial as its result. The caller moves on to the next
// call (nexttool).
static void resolve(Session *s, Arena scratch, b32 allow, i32 by, Str note)
{
    Engine *e = s->e;
    Ask    *k = &s->ask;
    Block  *c = &s->calls[s->callidx];
    i64     t = os_ticks(e->os);
    disarm(s, T_ASK);
    if (s->state == S_ASK) {
        setstate(s, S_IDLE);
    }

    Str why = note;
    if (!why.len) {
        switch (by) {
        case DENY_POLICY:  why = "unattended: nobody can answer";       break;
        case DENY_TIMEOUT: why = "nobody answered before the deadline"; break;
        case DENY_STEER:   why = "the user sent a message instead";     break;
        case DENY_CANCEL:  why = "the turn was cancelled";              break;
        }
    }
    {
        Buf   b(&scratch, 512 + why.len);
        EvOut w = sbegin(s, &b, EV_ANSWER);
        putstrkey(&w.json, "req", k->req);
        putstrkey(&w.json, "decision", allow ? Str("allow") : Str("deny"));
        putstrkey(&w.json, "by", bynames[by]);
        putstrkey(&w.json, "note", why);
        ssend(s, &w);
    }

    toolstart(s, scratch);
    if (allow) {
        Args args = parseargs(&scratch, k->tool, c->args);
        runfile(s, scratch, k->tool, k->gp.path, &args);
        return;
    } else if (by == DENY_CANCEL) {
        toolend(s, scratch, "cancelled", "Not run: the user cancelled the turn.",
                0, 0, {}, 0);
        return;
    }
    GuardDenial d = {};
    d.tool = toolnames[k->tool];
    d.gp   = k->gp;
    d.cwd  = s->guard.cwd;
    d.hint = k->hint;
    d.by   = by;
    d.note = note;
    d.wait = (t - k->opened + SEC/2) / SEC;
    Str text = guarddenial(&scratch, &d);
    toolend(s, scratch, "denied", text, 0, 0, {}, 0);
}

// A call the guard does not allow by itself. The user is asked if
// attending, and otherwise it is denied at once. Returns 1 if the call
// finished, 0 if the session now waits for an answer.
static b32 askcall(Session *s, Arena scratch, i32 tool, GuardPath *gp)
{
    Engine   *e    = s->e;
    Block    *c    = &s->calls[s->callidx];
    b32       read = tool == TOOL_READ;
    GuardHint h    = {};
    if (!gp->diverged) {
        h = guardhint(e->os, &scratch, &s->guard, gp->path, read, tool==TOOL_WRITE);
    }

    Ask *k = &s->ask;
    *k = {};
    k->req         = clone(&s->perm, concat(&scratch, c->id, "/ask"));
    k->tool        = tool;
    k->gp.path     = clone(&s->perm, gp->path);
    k->gp.real     = gp->real.data ? clone(&s->perm, gp->real) : Str{};
    k->gp.diverged = gp->diverged;
    k->hint.path   = h.path.data ? clone(&s->perm, h.path) : Str{};
    k->hint.exists = h.exists;
    k->opened      = os_ticks(e->os);

    // A steer waiting for the next step answers this ask too: the user
    // has already said what to do instead
    b32 steer = s->steers.len > 0;
    b32 wait  = s->attended && !steer;
    i64 delay = cfgms(e->cfg, "guard", "", "ask_timeout", 0)*MS;
    if (wait) {
        // The deadline runs from now: the record's fsync, which a slow
        // disk may draw out, must not extend it
        arm(s, T_ASK, delay);
    }
    {
        Str   why = guardwhy(&scratch, toolnames[tool], gp, s->guard.cwd);
        Buf   b(&scratch, 512 + why.len);
        EvOut w = sbegin(s, &b, EV_ASK);
        putstrkey(&w.json, "req", k->req);
        putstrkey(&w.json, "call", c->id);
        putstrkey(&w.json, "kind", "path");
        putstrkey(&w.json, "tool", toolnames[tool]);
        putstrkey(&w.json, "path", gp->path);
        putstrkey(&w.json, "detail", why);
        if (h.path.data) {
            putstrkey(&w.json, "hint", h.path);
        }
        putintkey(&w.json, "deadline", os_time(e->os) + (wait ? delay/MS : 0));
        putstrkey(&w.json, "dflt", "deny");
        ssend(s, &w);
    }
    if (!wait) {
        resolve(s, scratch, 0, steer ? DENY_STEER : DENY_POLICY, {});
        return 1;
    }
    setstate(s, S_ASK);
    return 0;
}

static b32 startshell(Session *s, Arena scratch, Args *args)
{
    Engine  *e  = s->e;
    Block   *c  = &s->calls[s->callidx];
    ShellRun sh = {};
    sh.timeout = args->timeout>0 ? args->timeout : SHELL_DEFAULT;
    sh.start   = os_ticks(e->os);
    // out/<call>.log, unless a model reused the call id in this session
    for (i32 n = 1;; n++) {
        Buf rel(&scratch, 128);
        print(&rel, "out/");
        print(&rel, c->id);
        if (n > 1) {
            putbyte(&rel, '-');
            print(&rel, (i64)n);
        }
        print(&rel, ".log");
        Str path = join(&scratch, s->dir, finish(&rel));
        if (os_stat(e->os, scratch, path).type == STAT_MISSING) {
            sh.rel   = clone(&s->perm, finish(&rel));
            sh.spill = clone(&s->perm, path);
            break;
        }
    }
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

// Run one call. Returns 1 if it finished, 0 if it goes on (a command, or
// an ask waiting for the user).
static b32 runtool(Session *s, Arena scratch)
{
    Engine *e    = s->e;
    Block  *c    = &s->calls[s->callidx];
    i32     tool = toolfind(c->name);
    Args    args = {};
    if (tool != TOOL_NONE) {
        args = parseargs(&scratch, tool, c->args);
    }

    // The guard comes first: a path it questions is asked about before
    // the call starts
    if (!s->blocked && tool!=TOOL_NONE && tool!=TOOL_SHELL && !args.issues.len) {
        GuardPath g = guardpath(e->os, &scratch, &s->guard, args.path, tool==TOOL_READ);
        if (!g.inside) {
            return askcall(s, scratch, tool, &g);
        }
        toolstart(s, scratch);
        runfile(s, scratch, tool, g.path, &args);
        return 1;
    }

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

    if (tool == TOOL_NONE) {
        Str text = unknowntool(&scratch, c->name);
        toolend(s, scratch, "error", text, 0, 0, {}, 0);
        return 1;
    }
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


// Compaction (compact.cpp)

static void countbytes(Session *s)
{
    s->bytes = 0;
    for (iz i = 0; i < s->history.len; i++) {
        s->bytes += msgbytes(&s->history[i]);
    }
}

// Replace the history before cutseq by a checkpoint with the summary,
// numbered seq, as compacting does, and replaying its record.
static void summarize(Session *s, Str summary, i64 cutseq, i64 seq)
{
    Msg ck = {};
    ck.role = ROLE_USER;
    ck.text = checkpoint(&s->perm, summary);
    ck.seq  = seq;
    Slice<Msg> h = {};
    h = push(&s->perm, h, ck);
    for (iz i = 0; i < s->history.len; i++) {
        if (s->history[i].seq >= cutseq) {
            h = push(&s->perm, h, s->history[i]);
        }
    }
    s->history = h;
    s->summary = summary;
    countbytes(s);
}

// Replace a tool result's text, as pruning does.
static void elide(Session *s, i64 seq, Str text)
{
    for (iz i = 0; i < s->history.len; i++) {
        Msg *m = &s->history[i];
        if (m->seq==seq && m->role==ROLE_TOOL) {
            m->text = text;
        }
    }
}

// Where a pruned result's full output is: the command's spill file, or
// for any other tool a file of the result itself, written now, in the
// session's out/ directory. Null if none could be had.
static Str fulloutput(Session *s, Arena *a, Arena scratch, Msg *m)
{
    Engine *e = s->e;
    if (m->spill.len) {
        return join(a, s->dir, m->spill);
    }
    for (i32 n = 1; n <= 100; n++) {
        Buf rel(&scratch, 128);
        print(&rel, "out/");
        print(&rel, m->callid.len ? m->callid : Str("result"));
        if (n > 1) {
            putbyte(&rel, '-');
            print(&rel, (i64)n);
        }
        print(&rel, ".log");
        Str path = join(&scratch, s->dir, finish(&rel));
        if (os_stat(e->os, scratch, path).type == STAT_MISSING) {
            return os_write(e->os, scratch, path, m->text) ? clone(a, path) : Str{};
        }
    }
    return {};
}

static void compactevent(Session *s, EvOut *w, Str kind, b32 overflow,
                         i64 before)
{
    putintkey(&w->json, "turn", s->turn);
    putstrkey(&w->json, "kind", kind);
    putstrkey(&w->json, "why", overflow ? Str("overflow") : Str("threshold"));
    putintkey(&w->json, "before", before);
    putintkey(&w->json, "after", estimate(s));
}

// Elide old tool outputs (pickprune), logging which, so that a replay
// elides them alike. Returns 1 if it did.
static b32 prune(Session *s, Arena scratch, b32 overflow)
{
    Prune pr = pickprune(&scratch, s->history, s->rate);
    if (!pr.idx.len) {
        return 0;
    }
    i64        before = estimate(s);
    Slice<iz>  done   = {};
    for (iz i = 0; i < pr.idx.len; i++) {
        Msg *m    = &s->history[pr.idx[i]];
        Str  path = fulloutput(s, &s->perm, scratch, m);
        if (path.len) {
            m->text = elision(&s->perm, tokensof(s->rate, m->text.len), path);
            done    = push(&scratch, done, pr.idx[i]);
        }
    }
    if (!done.len) {
        return 0;
    }
    countbytes(s);

    Buf   b(&scratch, 256 + 256*done.len);
    EvOut w = sbegin(s, &b, EV_COMPACT);
    compactevent(s, &w, "prune", overflow, before);
    putkey(&w.json, "elided");
    putarray(&w.json);
    for (iz i = 0; i < done.len; i++) {
        Msg *m = &s->history[done[i]];
        putobject(&w.json);
        putintkey(&w.json, "seq", m->seq);
        putstrkey(&w.json, "text", m->text);
        putend(&w.json);
    }
    putend(&w.json);
    ssend(s, &w);
    return 1;
}

// The compactor: [model] compactor, MODEL at the session's endpoint or
// ENDPOINT/MODEL, else the session's own model.
static void compactor(Session *s, Arena scratch, Compact *cp)
{
    Engine *e     = s->e;
    Str     spec  = trim(cfgget(e->cfg, "model", s->model, "compactor", 0));
    Str     which = s->ep.name.len ? s->ep.name : s->ep.url;
    Str     model = spec.len ? spec : s->model;
    Cut     c     = cut(spec, '/');
    if (c.ok && c.tail.len && trim(cfgget(e->cfg, "endpoint", c.head, "url", 0)).len) {
        which = c.head;
        model = c.tail;
    }
    if (!endpointconf(e, &s->perm, which, &cp->ep)) {
        cp->ep = s->ep;
        model  = s->model;
    }
    cp->model = clone(&s->perm, model);
    if (cp->model==s->model && cp->ep.url==s->ep.url) {
        cp->mc = s->mc;  // with what discovery learned
        return;
    }
    modelconf(e, &s->perm, cp->model, &cp->mc);
    i64 *k = upsert(&e->kinds, kindkey(&scratch, cp->ep.url, cp->model), (Arena *)0);
    cp->mc.llamacpp = k && (*k&1);
}

// Make room in the context: prune old tool outputs, and if the prompt
// is still not under enough tokens, summarize the older messages so
// that it comes to goal. The step goes on (proceed) once there is room,
// or else the turn ends saying why.
static void compact(Session *s, Arena scratch, i64 enough, i64 goal, b32 overflow)
{
    Engine *e = s->e;
    if (prune(s, scratch, overflow) && estimate(s)<enough) {
        proceed(s, scratch);
        return;
    }

    // What stays verbatim: the newest messages, up to what the goal
    // leaves after the fixed prompt and the summary
    i64 before = estimate(s);
    iz  start  = s->summary.len ? 1 : 0;
    i64 fixed  = tokensof(s->rate, s->system.len + s->tools.len);
    i64 keep   = goal - fixed - sumbudget(goal);
    keep = keep<COMPACT_KEEP ? keep : COMPACT_KEEP;
    iz  kept   = keep>0 ? cutpoint(s->history, start, s->rate, keep) : s->history.len;
    if (fixed+512>=goal || kept<=start) {
        Buf b(&scratch, 256);
        print(&b, overflow ? Str("overflow: the") : Str("the"));
        print(&b, " conversation, about ");
        print(&b, before);
        print(&b, " tokens, must come under ");
        print(&b, goal);
        print(&b, ", and compacting cannot make that much room");
        Str err = finish(&b);
        endturn(s, scratch, "error", err);
        return;
    }

    Compact cp = {};
    compactor(s, scratch, &cp);
    cp.goal     = goal;
    cp.overflow = overflow;
    cp.before   = before;
    cp.start    = start;
    cp.cut      = kept;
    cp.began    = os_ticks(e->os);
    s->cp       = cp;
    s->purpose  = REQ_COMPACT;
    s->retries  = 0;
    request(s, scratch);
}

static void sendcompact(Session *s, Arena scratch)
{
    static i32 const pct[] = {70, 50, 35};
    Compact *cp    = &s->cp;
    iz       limit = cp->shrink ? cp->rejected*pct[cp->shrink-1]/100 : 0;
    newattempt(s);

    // The request goes into the attempt arena, temporaries into scratch
    Str prev  = summarytext(s->summary);
    Str convo = transcript(&s->attempt, scratch, s->history, cp->start,
                           cp->cut, limit);
    i64 words = sumbudget(cp->goal) * 3/4;
    Str body  = encodesummary(&s->attempt, scratch, &cp->mc, prev, convo, words);
    Str url   = apiurl(&scratch, cp->ep.url, "chat/completions");
    cp->inbytes = convo.len;
    if (!startreq(s, scratch, &cp->ep, "POST", url, body, "text/event-stream", 0)) {
        Str err = os_lasterr(s->e->os, &scratch);
        err = concat(&scratch, "cannot start a request: ", err);
        endturn(s, scratch, "error", err);
        return;
    }
    arm(s, T_FIRSTBYTE, cp->ep.firstbyte);
    setstate(s, S_HEAD);
}

// The summary arrived: rebuild the history around it, log it, and carry
// on with the step, unless the prompt is still above the goal after a
// compaction at the threshold (the progress guard): compacting again
// would make no headway, so the turn ends instead.
static void compacted(Session *s, Arena scratch, Partial *p)
{
    Engine *e    = s->e;
    Compact cp   = s->cp;
    Str     text = trim(summarytext(trim(finish(&p->text))));
    if (!text.len) {
        failcompact(s, scratch, CLASS_TRANSIENT, 0, "the summary was empty", -1);
        return;
    }
    iz most = bytesof(s->rate, COMPACT_SUMMAX);
    text = text.len>most ? utf8head(text, most) : text;
    Files f = summaryfiles(&scratch, s->summary);
    f = addfiles(&scratch, f, s->history, cp.start, cp.cut);
    Str summary = withfiles(&s->perm, text, f);
    i64 cutseq  = cp.cut<s->history.len ? s->history[cp.cut].seq : s->log.seq+1;
    summarize(s, summary, cutseq, s->log.seq+1);

    Buf   b(&scratch, 512 + summary.len*2);
    EvOut w = sbegin(s, &b, EV_COMPACT);
    compactevent(s, &w, "summary", cp.overflow, cp.before);
    putintkey(&w.json, "cutseq", cutseq);
    putstrkey(&w.json, "summary", summary);
    putstrkey(&w.json, "endpoint", cp.ep.url);
    putstrkey(&w.json, "model", cp.model);
    putintkey(&w.json, "ms", (os_ticks(e->os) - cp.began)/MS);
    ssend(s, &w);
    s->purpose = REQ_CHAT;
    s->cp      = {};
    setstate(s, S_IDLE);

    i64 after = estimate(s);
    if (!cp.overflow && after>cp.goal) {
        Buf t(&scratch, 512);
        print(&t, "Compacting left the conversation at about ");
        print(&t, after);
        print(&t, " tokens, above 80% of the ");
        print(&t, compactat(s->mc.ctx));
        print(&t, " tokens at which it compacts, so the turn ends here "
                  "rather than compact over and over. Start a new session, "
                  "or use a model with a larger context.");
        Str why = finish(&t);
        note(s, scratch, "error", why);
        endturn(s, scratch, "error", "compacting did not make enough room");
        return;
    }
    proceed(s, scratch);
}


// Steps and turns

static void finalize(Session *s, Arena scratch)
{
    Partial *p = s->part;
    stopreq(s);
    s->part     = 0;
    s->slowfrom = 0;  // a response: the outage, if any, is over
    s->slown    = 0;
    if (s->purpose == REQ_COMPACT) {
        compacted(s, scratch, p);
        return;
    }
    Finished f  = finishpartial(&s->perm, p, 1, s->turn, s->steps);
    Str      fr = p->finish.data ? p->finish : Str("stop");
    addassistant(s, scratch, f.blocks, fr, p->hasusage ? &p->usage : 0);
    setstate(s, S_IDLE);

    // The router has just loaded the model, if it was not loaded: its
    // listing now reports the context, so look again before the next step
    if (s->info.llamacpp && !s->info.learned) {
        s->discovered = 0;
    }

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
        } else if (announces(finish(&p->text))) {
            // It said it would go on, but stopped: remind it, a few
            // times, and past that let the turn end as it chose
            Str text = "Your response says you will continue, but it ended "
                       "without a tool call, so nothing was done. If there is "
                       "more to do, do it now with a tool call; if the task "
                       "is finished, give your final answer.";
            if (!nudge(s, scratch, N_STOP, text)) {
                note(s, scratch, "warn", "the agent kept announcing work it "
                     "did not do, so its turn ends here");
                endturn(s, scratch, "done", {});
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

    s->overflows = 0;
    s->ovsize    = 0;
    if (s->target && os_ticks(s->e->os)>=s->homeat) {
        gohome(s, scratch, "its cooldown is over");
    }
    proceed(s, scratch);
}

// A session's title from its first prompt: one line, each run of
// whitespace a single space, cut near TITLE_MAX code points on a word
// boundary (unless that would lose half of it) with an ellipsis.
static Str maketitle(Arena *a, Str text)
{
    Buf b(a, 4*TITLE_MAX + 8);
    iz  n     = 0;   // code points so far
    iz  sp    = -1;  // b.len at the last space
    iz  spn   = 0;   // n there
    b32 space = 0;
    b32 more  = 0;
    for (iz i = 0; i < text.len;) {
        iz  start = i;
        i32 c     = utf8decode(text, &i);
        if (c<0x20 || c==' ' || c==0x7f || c==0xa0) {
            space = b.len > 0;
            continue;
        }
        if (n+space+1 > TITLE_MAX) {
            more = 1;
            break;
        }
        if (space) {
            sp    = b.len;
            spn   = n;
            space = 0;
            putbyte(&b, ' ');
            n++;
        }
        print(&b, span(text.data+start, text.data+i));
        n++;
    }
    if (more) {
        if (!space && sp>=0 && spn>=TITLE_MAX/2) {
            b.len = sp;  // within a word: back to its start
        }
        print(&b, "\xe2\x80\xa6");  // ellipsis
    }
    return finish(&b);
}

static void settitle(Session *s, Arena scratch, Str title)
{
    if (!title.len) {
        return;
    }
    s->title = clone(&s->perm, title);
    Buf   b(&scratch, 128 + title.len);
    EvOut w = sbegin(s, &b, EV_TITLE);
    putstrkey(&w.json, "text", title);
    ssend(s, &w);
    // For listing sessions without reading their logs
    Str path = join(&scratch, s->dir, "title");
    os_write(s->e->os, scratch, path, s->title);
}

static void startturn(Session *s, Arena scratch, Str text, Str via)
{
    s->cancelled = 0;
    adduser(s, scratch, text, via);
    if (!s->title.len) {
        Str title = maketitle(&scratch, text);
        settitle(s, scratch, title);
    }
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
    release(s, 0);
    stopreq(s);
    if (s->point) {
        admcool(s->point, s->slotmodel);  // no next step to keep it for
        pump(s->e, s->point);
    }
    for (i32 t = 0; t < T_COUNT; t++) {
        disarm(s, t);
    }
    s->part     = 0;
    s->purpose  = REQ_CHAT;
    s->cp       = {};
    s->slowing  = 0;
    s->slowfrom = 0;
    s->slown    = 0;
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
    case S_ASK:
        resolve(s, scratch, 0, DENY_CANCEL, {});
        nexttool(s, scratch);
        return;
    case S_HEAD:
    case S_STREAM:
    case S_ERRBODY:
        if (s->purpose==REQ_CHAT && s->part && committed(s->part)) {
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

// Resolve an endpoint by URL or by name. Settings come from the section
// of that name, or the section whose url matches, over the schema's
// defaults (config.cpp).
static b32 endpointconf(Engine *e, Arena *perm, Str which, Endpoint *ep)
{
    Config *cfg  = e->cfg;
    Str     name = {};
    Str     url  = which;
    if (!parseurl(which).ok) {
        name = which;
        url  = trim(cfgget(cfg, "endpoint", which, "url", 0));
    } else {
        for (IniSection *s = inifirst(e->ini, "endpoint"); s; s = ininext(s)) {
            if (iniget(e->ini, "endpoint", s->name, "url") == url) {
                name = s->name;
            }
        }
    }
    Url u = parseurl(url);
    if (!u.ok) {
        return 0;
    }
    b32 local = localhost(u.host);
    ep->name      = clone(perm, name);
    ep->url       = clone(perm, url);
    ep->firstbyte = cfgms(cfg, "endpoint", name, "first_byte_timeout", local)*MS;
    ep->idle      = cfgms(cfg, "endpoint", name, "idle_timeout", local)*MS;
    ep->progress  = cfgms(cfg, "endpoint", name, "progress_timeout", local)*MS;
    i64 connect   = cfgms(cfg, "endpoint", name, "connect_timeout", local);
    ep->connectms = (i32)(connect<0x7fffffff ? connect : 0x7fffffff);
    ep->key       = cfgsecret(cfg, perm, "endpoint", name, "key");
    return 1;
}

static void modelconf(Engine *e, Arena *perm, Str model, ModelConf *mc)
{
    static Str const sampling[] = {
        "temperature", "top_p", "top_k", "min_p", "presence_penalty",
        "frequency_penalty", "repeat_penalty", "seed", "max_tokens",
    };
    Config *cfg = e->cfg;
    mc->name = clone(perm, model);
    Str r = trim(cfgget(cfg, "model", model, "reasoning", 0));
    mc->reasoning = r=="none" || r=="never" ? Str{} : clone(perm, r);
    for (iz i = 0; i < countof(sampling); i++) {
        Str v = trim(cfgget(cfg, "model", model, sampling[i], 0));
        if (v.len && parsefloat(v).ok) {
            Extra x = {sampling[i], clone(perm, v)};
            mc->extras = push(perm, mc->extras, x);
        }
    }
    Int ctx = parseint(trim(cfgget(cfg, "model", model, "context", 0)));
    mc->ctx = ctx.ok && ctx.val>0 ? ctx.val : 0;
    Int rb  = parseint(trim(cfgget(cfg, "model", model, "reasoning_budget", 0)));
    mc->thinkmax = rb.ok && rb.val>0 ? rb.val : 0;
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
    s->log.fd  = -1;
    s->lock    = -1;
    return s;
}

// Forget a session, listed or not, releasing its lock and memory.
static void dropsession(Engine *e, Session *s)
{
    for (Session **p = &e->sessions; *p; p = &(*p)->next) {
        if (*p == s) {
            *p = s->next;
            break;
        }
    }
    logclose(&s->log);
    if (s->lock >= 0) {
        os_unlockfile(e->os, s->lock);
    }
    Region *r1 = s->perm.region;
    os_release(e->os, s->aregion);
    os_release(e->os, r1);  // holds s itself
}

static Str lockpath(Arena *a, Session *s)
{
    return join(a, s->dir, "lock");
}

// The session's workspace: its working directory, its own extra roots,
// and those of the configuration ("[guard] roots = A; B").
static Guard sessionguard(Session *s, Arena *perm)
{
    Engine    *e     = s->e;
    Slice<Str> roots = {};
    Slice<Str> conf  = cfgpaths(e->cfg, perm, "guard", "", "roots", e->home);
    for (iz i = 0; i < s->roots.len; i++) {
        roots = push(perm, roots, s->roots[i]);
    }
    for (iz i = 0; i < conf.len; i++) {
        roots = push(perm, roots, conf[i]);
    }
    return newguard(e->os, perm, s->cwd, e->home, roots, s->dir, s->te.windows);
}

static void sessionready(Session *s)
{
    Engine *e = s->e;
    s->home      = s->ep.name.len ? s->ep.name : s->ep.url;
    s->homemodel = s->model;
    s->system    = systemprompt(&s->perm, s->cwd, &s->te);
    s->tools  = tooljson(&s->perm, &s->te);
    s->guard  = sessionguard(s, &s->perm);
    // Tell the agent about the other places it may work
    Buf b(&s->perm, 256);
    for (iz i = 1; i < s->guard.roots.len; i++) {
        GuardRoot *t = &s->guard.roots[i];
        if (!t->read) {
            print(&b, "\n- ");
            print(&b, t->lex);
        }
    }
    if (b.len) {
        Str list  = finish(&b);
        s->system = concat(&s->perm, s->system,
                           "\n\nBesides the working directory, the file "
                           "tools may also use these directories:");
        s->system = concat(&s->perm, s->system, list);
    }
    s->next = e->sessions;
    e->sessions = s;
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

// A flag in a command: true or false, or a number, nonzero for true.
static b32 jsonflag(Json *j, b32 dflt)
{
    return j->type==JSON_BOOL ? j->boolean : jsonint(j, dflt)!=0;
}

static void cmdnew(Engine *e, Arena scratch, i64 id, Json *j)
{
    Str cwd   = jsonstr(jsonget(j, "cwd"));
    Str model = jsonstr(jsonget(j, "model"));
    Str which = jsonstr(jsonget(j, "endpoint"));
    if (!which.len) which = iniget(e->ini, "", "", "endpoint");
    if (!model.len) model = iniget(e->ini, "", "", "model");
    Slice<Str> roots = {};
    for (Json *r = jsonget(j, "roots")->child; r; r = r->next) {
        Str root = jsonstr(r);
        if (root.len) {
            roots = push(&scratch, roots, root);
        }
    }

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
    s->cwd      = clone(&s->perm, real);
    s->model    = clone(&s->perm, model);
    s->attended = jsonflag(jsonget(j, "attended"), 0);
    for (iz i = 0; i < roots.len; i++) {
        s->roots = push(&s->perm, s->roots, clone(&s->perm, roots[i]));
    }
    endpointconf(e, &s->perm, which, &s->ep);
    modelconf(e, &s->perm, model, &s->mc);

    if (!os_mkdirs(e->os, scratch, s->outdir)) {
        dropsession(e, s);
        reply(e, scratch, id, 0, "cannot create the session directory", {});
        return;
    }
    Str lock = lockpath(&scratch, s);
    s->lock = os_lockfile(e->os, scratch, lock);
    if (s->lock < 0) {
        dropsession(e, s);
        reply(e, scratch, id, 0, "cannot lock the new session", {});
        return;
    }
    Str path = join(&scratch, s->dir, "events.jsonl");
    s->log = logopen(e->os, &s->perm, scratch, path, s->sid, LOG_MAX);
    if (!s->log.ok) {
        dropsession(e, s);
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
        if (s->roots.len) {
            putkey(&w.json, "roots");
            putarray(&w.json);
            for (iz i = 0; i < s->roots.len; i++) {
                putstr(&w.json, s->roots[i]);
            }
            putend(&w.json);
        }
        ssend(s, &w);
    }
    reply(e, scratch, id, 1, {}, s->sid);

    Str prompt = jsonstr(jsonget(j, "prompt"));
    if (prompt.len) {
        startturn(s, scratch, prompt, "prompt");
    }
}

// Rebuild a session from its log: the history, the turn count, and the
// closing of anything a crash left open (logopen), including results for
// calls that never started. *firstnew is the seq before any repairs.
// When another process holds the session's lock, it is only read, and
// so neither repaired nor written later (readonly).
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
    Str lock = lockpath(&scratch, s);
    s->lock     = os_lockfile(e->os, scratch, lock);
    s->readonly = s->lock < 0;
    if (!s->readonly) {
        s->log = logopen(e->os, &s->perm, scratch, path, s->sid, LOG_MAX);
        if (!s->log.ok) {
            dropsession(e, s);
            return 0;
        }
        *firstnew = s->log.seq - s->log.synth;
    }

    Arena  lines = newarena(e->replay);
    Replay rp    = logreplay(e->os, &lines, scratch, path, s->sid, 0, LOG_MAX);
    if (s->readonly) {
        s->log.seq = rp.last;
        *firstnew  = rp.last;
    }
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
            for (Json *k = jsonget(j, "roots")->child; k; k = k->next) {
                Str root = clone(&s->perm, jsonstr(k));
                s->roots = push(&s->perm, s->roots, root);
            }
            break;
        case EV_TITLE:
            s->title = clone(&s->perm, jsonstr(jsonget(j, "text")));
            break;
        case EV_TURN:
            s->turn = jsonint(jsonget(j, "n"), s->turn);
            break;
        case EV_USER:
        case EV_NUDGE:
            m.role = ROLE_USER;
            m.text = clone(&s->perm, jsonstr(jsonget(j, "text")));
            m.seq  = r.seq;
            addmsg(s, m);
            break;
        case EV_COMPACT: {
            // As compacting rewrote the history then
            for (Json *x = jsonget(j, "elided")->child; x; x = x->next) {
                Str text = clone(&s->perm, jsonstr(jsonget(x, "text")));
                elide(s, jsonint(jsonget(x, "seq"), 0), text);
            }
            Json *sum = jsonget(j, "summary");
            if (sum->type == JSON_STRING) {
                Str summary = clone(&s->perm, jsonstr(sum));
                summarize(s, summary, jsonint(jsonget(j, "cutseq"), 0), r.seq);
            }
            countbytes(s);
        } break;
        case EV_ASSISTANT:
            m.seq  = r.seq;
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
            m.spill  = clone(&s->perm, jsonstr(jsonget(j, "spill")));
            m.seq    = r.seq;
            addmsg(s, m);
            break;
        }
    }
    if (!s->cwd.len) {
        dropsession(e, s);
        return 0;
    }
    s->model = model.len ? clone(&s->perm, model) : storedmodel;
    if (!endpointconf(e, &s->perm, which.len ? which : storedurl, &s->ep)) {
        dropsession(e, s);
        return 0;
    }
    modelconf(e, &s->perm, s->model, &s->mc);
    sessionready(s);
    return s;
}

// What streamed of the current response so far, for a frontend opening
// the session mid-stream: each block as one delta would carry all of it,
// in order, which the deltas that follow extend. A text longer than its
// share of the budget keeps its end, and says how much was cut. The
// budget leaves the record well inside the frontend's mailbox even with
// every byte escaped, so that it never overflows by itself.
static void sendsnapshot(Session *s, Arena scratch)
{
    Partial *p = s->part;
    if (!p || !requesting(s->state) || s->purpose!=REQ_CHAT) {
        return;
    }
    iz budget = mailmax(s->e->out) / 8;
    if (budget > SNAPSHOT_MAX) {
        budget = SNAPSHOT_MAX;
    }
    iz share = budget / (p->nblocks>0 ? p->nblocks : 1);

    Buf   b(&scratch, 1024 + budget);
    EvOut w = sbegin(s, &b, EV_SNAPSHOT);
    putintkey(&w.json, "turn", s->turn);
    putintkey(&w.json, "attempt", s->attempts);
    putkey(&w.json, "blocks");
    putarray(&w.json);
    for (i32 i = 0; i < p->nblocks; i++) {
        Str       kind = {};
        Str       text = {};
        PartCall *c    = 0;
        if (i == p->thinkblock) {
            kind = "think";
            text = finish(&p->think);
        } else if (i == p->textblock) {
            kind = "text";
            text = finish(&p->text);
        } else {
            for (iz k = 0; k < p->calls.len; k++) {
                c = p->calls[k].block==i ? &p->calls[k] : c;
            }
            if (!c) {
                continue;
            }
            kind = "args";
            text = finish(&c->args);
        }
        iz cut = 0;
        if (text.len > share) {
            cut = text.len - share;
            for (; cut<text.len && (text[cut]&0xc0)==0x80; cut++) {}
            text = cuthead(text, cut);
        }
        putobject(&w.json);
        putintkey(&w.json, "block", i);
        putstrkey(&w.json, "kind", kind);
        if (c) {
            putstrkey(&w.json, "id", finish(&c->id));
            putstrkey(&w.json, "name", finish(&c->name));
        }
        putstrkey(&w.json, "text", text);
        if (cut) {
            putintkey(&w.json, "cut", cut);
        }
        putend(&w.json);
    }
    putend(&w.json);
    ssend(s, &w);
}

static void cmdopen(Engine *e, Arena scratch, i64 id, Json *j)
{
    Str      sid   = jsonstr(jsonget(j, "sid"));
    i64      from  = jsonint(jsonget(j, "from"), 0);
    Session *s     = findsession(e, sid);
    i64      first = I64MAX;
    if (s && s->readonly) {
        Str lock = lockpath(&scratch, s);
        if (!os_lockheld(e->os, scratch, lock)) {
            dropsession(e, s);  // the other process let go: open it for real
            s = 0;
        }
    }
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
    Json *att = jsonget(j, "attended");
    if (att->type != JSON_MISSING) {
        s->attended = jsonflag(att, 0);
        if (!s->attended && s->state==S_ASK) {
            // Nobody is left to answer
            resolve(s, scratch, 0, DENY_POLICY, "the session is no longer attended");
            nexttool(s, scratch);
        }
    }
    sendsnapshot(s, scratch);

    Buf   b(&scratch, 256);
    EvOut w = evbegin(&b, s->sid, 0, os_time(e->os), EV_REPLY);
    putintkey(&w.json, "id", id);
    putintkey(&w.json, "ok", 1);
    putintkey(&w.json, "readonly", s->readonly);
    emit(e, evend(&w));
    live(s);  // the state of a session that was open already
}

static void cmdlist(Engine *e, Arena scratch, i64 id)
{
    struct Entry {
        Str sid;
        Str title;
        Str cwd;
        Str model;
        i64 mtime;
        b32 locked;  // open in another process
    };
    Slice<Entry> list = {};
    Str dir = join(&scratch, e->statedir, "sessions");
    for (Dirent *d = os_list(e->os, &scratch, dir); d; d = d->next) {
        if (!d->isdir) continue;
        Str   sdir = join(&scratch, dir, d->name);
        Str   path = join(&scratch, sdir, "events.jsonl");
        Read  rd   = os_readat(e->os, &scratch, path, 0, 4096);
        Cut   line = cut(rd.data, '\n');
        EvRec r    = evparse(&scratch, line.head);
        if (!line.ok || r.ev!=EV_SESSION) continue;
        Session *s     = findsession(e, d->name);
        Str      title = join(&scratch, sdir, "title");
        Str      lock  = join(&scratch, sdir, "lock");
        Entry en = {};
        en.sid    = d->name;
        en.title  = os_read(e->os, &scratch, title, 1024).data;
        en.cwd    = jsonstr(jsonget(r.root, "cwd"));
        en.model  = jsonstr(jsonget(r.root, "model"));
        en.mtime  = d->mtime;
        en.locked = (!s || s->readonly) && os_lockheld(e->os, scratch, lock);
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
        putstrkey(&w.json, "title", list[i].title);
        putstrkey(&w.json, "cwd", list[i].cwd);
        putstrkey(&w.json, "model", list[i].model);
        putstrkey(&w.json, "state", live ? statenames[live->state] : Str("closed"));
        putintkey(&w.json, "mtime", list[i].mtime);
        putintkey(&w.json, "locked", list[i].locked);
        putend(&w.json);
    }
    putend(&w.json);
    emit(e, evend(&w));
    reply(e, scratch, id, 1, {}, {});
}

// After the configuration reloads: each session takes up its endpoint's
// and model's settings for its next request, and the guard's roots for
// its next call. The system prompt stays as it was, byte for byte, to
// keep the prompt cache: it names only the roots the session began with.
static void reconfigure(Engine *e)
{
    e->ini = e->cfg->ini;
    for (Session *s = e->sessions; s; s = s->next) {
        s->guard = sessionguard(s, &s->perm);
        Str      which = s->ep.name.len ? s->ep.name : s->ep.url;
        Endpoint ep    = {};
        if (endpointconf(e, &s->perm, which, &ep)) {
            s->ep = ep;
        }
        ModelConf mc = {};
        modelconf(e, &s->perm, s->model, &mc);
        mc.llamacpp = s->mc.llamacpp;  // learned by discovery
        mc.ctx      = mc.ctx ? mc.ctx : s->mc.ctx;
        s->mc = mc;
    }
}

// config.get and config.set, with the shapes in config.cpp.
static void cmdconfig(Engine *e, Arena scratch, i64 id, i32 cmd, Json *j)
{
    Config *c   = e->cfg;
    CfgSet  r   = {};
    i64     rev = c->rev;
    if (cmd == CMD_CONFIGGET) {
        r.ok = 1;
        cfgpoll(c, scratch, 1);
    } else {
        Json *v     = jsonget(j, "value");
        Str   value = v->type==JSON_STRING ? (v->str.data ? v->str : Str("")) :
                      Str{};
        if (v->type!=JSON_STRING && v->type!=JSON_NULL) {
            r.err = "value must be a string, or null to remove the key";
        } else {
            r = cfgset(c, &scratch, jsonstr(jsonget(j, "kind")),
                       jsonstr(jsonget(j, "name")), jsonstr(jsonget(j, "key")),
                       value);
        }
    }
    if (c->rev != rev) {
        reconfigure(e);
    }

    Buf   b(&scratch, 1<<14);
    EvOut w = evbegin(&b, {}, 0, os_time(e->os), EV_REPLY);
    putintkey(&w.json, "id", id);
    putintkey(&w.json, "ok", r.ok);
    if (!r.ok) {
        putstrkey(&w.json, "err", r.err);
    }
    if (cmd == CMD_CONFIGGET) {
        putcfg(&w.json, c);
    } else {
        putcfgset(&w.json, c, &r);
    }
    emit(e, evend(&w));
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
    if (e->quitting) {
        // Nothing new starts once stopping, so that the engine can stop
        switch (cmd) {
        case CMD_NEW:
        case CMD_OPEN:
        case CMD_PROMPT:
        case CMD_STEER:
        case CMD_ANSWER:
            reply(e, scratch, id, 0, "the engine is stopping", sid);
            return;
        }
    }
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
    case CMD_QUIT: {
        // Stop: cancel every turn, killing their tools, then wait a
        // while for each to end (enginethread) before stopping
        Str mode = jsonstr(jsonget(j, "mode"));
        if (mode.len && mode!="stop") {
            reply(e, scratch, id, 0, "unknown quit mode: only \"stop\" exists", {});
            return;
        }
        if (!e->quitting) {
            e->quitting = 1;
            e->quitby   = os_ticks(e->os) + QUIT_WAIT;
        }
        for (Session *t = e->sessions; t; t = t->next) {
            cancelturn(t, scratch);
        }
        reply(e, scratch, id, 1, {}, {});
    } return;
    case CMD_CONFIGGET:
    case CMD_CONFIGSET:
        cmdconfig(e, scratch, id, cmd, j);
        return;
    case CMD_NONE:
        reply(e, scratch, id, 0, "unknown command", {});
        return;
    }

    if (!s) {
        reply(e, scratch, id, 0, "no such open session", sid);
        return;
    } else if (s->readonly && cmd!=CMD_CLOSE) {
        reply(e, scratch, id, 0, "read-only: another process has this "
              "session open; open it again once that one has closed it", sid);
        return;
    }
    switch (cmd) {
    case CMD_CLOSE:
        // Nobody is left to answer asks
        reply(e, scratch, id, 1, {}, sid);
        s->attended = 0;
        if (s->state == S_ASK) {
            resolve(s, scratch, 0, DENY_POLICY, "the user closed the session");
            nexttool(s, scratch);
        }
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
            // Read at the next step; a pending ask yields to it, as do
            // any more asks before then (askcall)
            s->steers = push(&s->perm, s->steers, clone(&s->perm, text));
            if (s->state == S_ASK) {
                resolve(s, scratch, 0, DENY_STEER, {});
                nexttool(s, scratch);
            }
        }
        return;
    case CMD_CANCEL:
        reply(e, scratch, id, 1, {}, sid);
        cancelturn(s, scratch);
        return;
    case CMD_ANSWER: {
        Str req      = jsonstr(jsonget(j, "req"));
        Str decision = jsonstr(jsonget(j, "decision"));
        if (s->state != S_ASK) {
            reply(e, scratch, id, 0, "nothing is waiting for an answer", sid);
        } else if (req != s->ask.req) {
            Str err = concat(&scratch, "no such request: the one waiting is ", s->ask.req);
            reply(e, scratch, id, 0, err, sid);
        } else if (decision!="allow" && decision!="deny") {
            reply(e, scratch, id, 0, "decision must be \"allow\" or \"deny\"", sid);
        } else {
            reply(e, scratch, id, 1, {}, sid);
            Str note = jsonstr(jsonget(j, "note"));
            resolve(s, scratch, decision=="allow", DENY_USER, note);
            nexttool(s, scratch);
        }
    } return;
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

static void onrecord(Engine *e, Arena scratch, Record r)
{
    if (r.type == MAIL_CMD) {
        command(e, scratch, r.data);
        return;
    } else if (r.type == MAIL_TOOLONG) {
        reply(e, scratch, (i64)r.id, 0, "command too long", {});
        return;
    } else if (onprobe(e, scratch, r) || onctl(e, scratch, r)) {
        return;
    }

    b32      proc = r.type>=PROC_TAIL && r.type<=PROC_DATA;
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
        // Messages are complete before scratch is passed on, which copies
        // it: an argument allocated in it alongside would be overwritten
        msg = concat(&scratch, "no response within ", secstr(&scratch, reqep(s)->firstbyte));
        msg = concat(&scratch, msg, " s");
        fail(s, scratch, CLASS_TRANSIENT, 0, msg, -1);
        return;
    case T_IDLE:
        msg = concat(&scratch, "no data for ", secstr(&scratch, reqep(s)->idle));
        msg = concat(&scratch, msg, " s");
        fail(s, scratch, CLASS_TRANSIENT, 0, msg, -1);
        return;
    case T_PROGRESS:
        msg = concat(&scratch, "no progress for ", secstr(&scratch, reqep(s)->progress));
        msg = concat(&scratch, msg, " s");
        fail(s, scratch, CLASS_TRANSIENT, 0, msg, -1);
        return;
    case T_GRACE:
        finalize(s, scratch);
        return;
    case T_BACKOFF:
        if (s->slowing) {
            // After a slow wait, start over from the primary
            s->slowing = 0;
            gohome(s, scratch, "a slow retry");
            proceed(s, scratch);
        } else {
            request(s, scratch);
        }
        return;
    case T_ADMIT:
        sendreq(s, scratch);
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
    case T_ASK:
        resolve(s, scratch, 0, DENY_TIMEOUT, {});
        nexttool(s, scratch);
        return;
    case T_LIVE:
        live(s);
        arm(s, T_LIVE, LIVE_EVERY);
        return;
    }
}

static void enginethread(void *arg)
{
    Engine *e = (Engine *)arg;
    for (;;) {
        i64 now  = os_ticks(e->os);
        i64 next = e->quitting ? e->quitby : I64MAX;
        next = e->admat && e->admat<next ? e->admat : next;
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

        // Records mean work: catch up with configuration edits first
        Mail m = mailtake(e->in);
        if (m.len && cfgpoll(e->cfg, newarena(e->scratch), 0)) {
            reconfigure(e);
        }
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
        if (e->admat && e->admat<=now) {
            // Time alone lets a queued request go
            e->admat = 0;
            for (AdmPoint *p = e->adm.points; p; p = p->next) {
                pump(e, p);
            }
        }

        // Cancelling ends turns at once, but should a turn linger, the
        // engine stops anyway once the wait is over
        if (e->quitting && (allidle(e) || now>=e->quitby)) {
            for (Session *s = e->sessions; s; s = s->next) {
                logclose(&s->log);
                if (s->lock >= 0) {
                    os_unlockfile(e->os, s->lock);
                    s->lock = -1;
                }
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
    Str cmd  = concat(&scratch, root.len ? root : Str("C:\\Windows"), "\\System32\\cmd.exe");
    te.shell     = clone(perm, cmd);
    te.shellkind = SHELL_CMD;
    return te;
}

// An engine with its configuration (config.cpp), which from now on
// belongs to the engine thread: frontends use config.get and config.set.
static Engine *newengine(Os *os, Arena *perm, Config *cfg, Str statedir,
                         Mailbox *out)
{
    b32     big = sizeof(void *) == 8;
    Engine *e   = alloc<Engine>(perm);
    e->os       = os;
    e->out      = out;
    e->cfg      = cfg;
    e->ini      = cfg->ini;
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
    e->env     = os_environ(os, &e->perm);
    e->home    = os_getenv(os, &e->perm, "HOME");
    if (HOST_WINDOWS && !e->home.len) {
        e->home = os_getenv(os, &e->perm, "USERPROFILE");
    }
    loadtrained(e, newarena(e->scratch));
    // Tests shorten minutes of slow retries to milliseconds
    Int minute = parseint(os_getenv(os, &e->perm, "HARNESS_TEST_MINUTE_MS"));
    e->minute  = minute.ok && minute.val>0 && minute.val<=60000 ? minute.val*MS : 60*SEC;
    return e;
}

static b32 enginestart(Engine *e)
{
    return os_thread(e->os, enginethread, e);
}

// The id of a command, read without parsing it whole: the number of its
// top-level "id" key, or 0. A prefix of a line too long to take will do,
// if the id comes early enough.
static i64 cmdlineid(Str line)
{
    iz i = 0;
    for (; i<line.len && whitespace(line[i]); i++) {}
    if (i==line.len || line[i]!='{') {
        return 0;
    }
    for (i++;;) {
        for (; i<line.len && whitespace(line[i]); i++) {}
        if (i==line.len || line[i]!='"') {
            return 0;
        }
        iz k = ++i;
        for (; i<line.len && line[i]!='"'; i += line[i]=='\\' ? 2 : 1) {}
        if (i >= line.len) {
            return 0;
        }
        Str key = span(line.data+k, line.data+i);
        for (i++; i<line.len && whitespace(line[i]); i++) {}
        if (i==line.len || line[i]!=':') {
            return 0;
        }
        for (i++; i<line.len && whitespace(line[i]); i++) {}
        if (key == "id") {
            iz j = i;
            for (; j<line.len && (digit(line[j]) || line[j]=='-'); j++) {}
            Int n = parseint(span(line.data+i, line.data+j));
            return n.ok ? n.val : 0;
        }

        // Skip the value, through any strings and containers in it
        for (i32 depth = 0; i < line.len; i++) {
            u8 c = line[i];
            if (c == '"') {
                for (i++; i<line.len && line[i]!='"'; i += line[i]=='\\' ? 2 : 1) {}
                if (i >= line.len) {
                    return 0;
                }
            } else if (c=='{' || c=='[') {
                depth++;
            } else if ((c=='}' || c==']') && depth) {
                depth--;
            } else if ((c==',' || c=='}') && !depth) {
                break;
            }
        }
        if (i==line.len || line[i]!=',') {
            return 0;
        }
        i++;
    }
}

// Refuse a command too long to take, by its id: the engine replies.
static void enginetoolong(Engine *e, i64 id)
{
    mailpost(e->in, MAIL_TOOLONG, (u64)id, {}, 1);
}

// Post a command line from any thread. One too long for the mailbox is
// refused in its place, and then this returns 0.
static b32 enginecmd(Engine *e, Str line)
{
    if (line.len > mailmax(e->in)) {
        enginetoolong(e, cmdlineid(line));
        return 0;
    }
    return mailpost(e->in, MAIL_CMD, 0, line, 1);
}
