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
//   The todo nudges (todo items left at the end of a turn, many changes
//   without the list changing) reset only as the list moves along.
// - Tools: denials and invalid calls are tool results, never the end
//   of a turn. A command still running at its timeout moves to the
//   background as a job, which has a limit of its own; whatever a
//   command leaves behind is killed when it completes. A job's end is
//   told to the agent at its next request, and an idle session starts
//   a turn for it, a few times an hour at most. Stopping a command asks
//   first (TERM, or CTRL_BREAK), then kills after a grace period.
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
// - Subagents: the task tool runs a task in a session of its own, under
//   a role, concurrently with the rest, and its report is the call's
//   result. A subagent has a budget of requests, may go two levels
//   deep, and is cancelled with its parent (see "Roles and subagents").
// - Parking: a session idle for 7 minutes gives up its memory, and its
//   next command revives it from its log (see "Idle parking").
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
    FILECMD_MAX  = 1 << 20,       // bytes of a file a file command gives
    PARK_IDLE    = 7*60*SEC,      // idle this long, a session is parked
    TASK_SOFT    = 200,           // a subagent's requests, by default
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
    S_TASK,      // waiting for subagents to report
};

static Str statenames[] = {
    "idle", "discover", "waiting", "streaming", "error", "backoff", "tool",
    "ask", "queued", "task",
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
    T_JOBS,      // the next deadline of a background job
    T_TASK,      // a subagent reported: end the calls that may end
    T_PARK,      // idle so long: free its memory
    T_DESK,      // the desktop helper's answer is due
    T_COUNT,
};

enum : i32 {
    N_CONTINUE,  // after an interrupted response
    N_EMPTY,     // nothing visible and no calls
    N_LENGTH,    // stopped at the length limit
    N_LOOP,      // a text cycle
    N_GUARD,     // the same calls 5 times
    N_STOP,      // a final response that announced more work
    // Reset only by todo progress and new input, not by tool progress
    N_TODO,      // the turn ends with todo items not done
    N_REMIND,    // many changes without a todo update
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
    {"todo",     3},
    {"todo_reminder", 2},
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

enum : i64 {
    TODO_REMIND = 12,  // mutating calls without a todo update
};

// Something for the agent to read before its next request: a job that
// ended, files rewound, instructions for a directory.
struct Notice {
    Str kind;  // "job", "rewind", "instructions"
    Str text;
    i64 job;   // for "job"
};

enum : i32 {
    JOB_RUN,   // running
    JOB_STOP,  // asked to stop: the kill follows at killat
    JOB_DONE,
};

enum : i32 {
    JOBEND_EXIT,     // it ended by itself
    JOBEND_TIMEOUT,  // stopped at its deadline
    JOBEND_KILLED,   // by job_kill or job.kill
    JOBEND_CLOSED,   // the session closed, or the engine stopped
    JOBEND_LIMIT,    // too much output
    JOBEND_LOST,     // the harness stopped while it ran (logopen)
    JOBEND_COUNT,
};

static Str jobendnames[] = {
    "exited", "timeout", "killed", "closed", "limit", "unknown",
};
static_assert(countof(jobendnames) == JOBEND_COUNT);

// A command in the background: one moved there at its timeout, one
// started with background, or a cancelled command being stopped
// (hidden: never shown to the agent, nor logged).
struct Job {
    Job  *next;
    i64   n;         // number within the session, from 1
    Str   call;
    Str   command;
    Str   spill;     // absolute
    Str   rel;       // relative to the session directory
    Proc *proc;
    u64   procid;
    i64   start;     // ticks
    i64   deadline;  // ticks: stopped then
    i64   killat;    // ticks: killed then, once stopping
    i64   drainat;   // ticks: done waiting for its output, once exited
    i64   total;
    i32   code;
    i32   signal;
    i32   state;     // JOB_*
    i32   why;       // JOBEND_*
    b32   exited;
    b32   eof;
    b32   hidden;
    b32   quiet;     // the agent stopped it: no notice
    b32   cut;       // processes it left held its output
    i64   ran;       // ticks it ran, once done
};

// The original of a file the harness changed, kept as snap/<n> in the
// session directory, for rewind.
struct Snap {
    i64 n;
    i64 seq;      // of the snap record
    i64 turn;
    Str call;
    Str path;     // absolute and normalized, as the tool used it
    Str before;   // hash of the original, empty if the file was absent
    Str after;    // hash of what the harness wrote
    b32 existed;
    b32 undone;   // rewound already
};

// Where a call started, for rewinding to it.
struct CallMark {
    Str call;
    i64 seq;
    i64 turn;
};

// An instruction file loaded into the session, and how it was then.
struct InstrStamp {
    Str path;
    i64 size;
    i64 mtime;
};

// A task call and its subagent, while the calls of its response run.
struct TaskRun {
    iz  call;    // index into the session's calls
    Str child;   // the subagent's sid, null if none started
    i64 start;   // ticks
    b32 done;    // reported: the result is below
    Str status;  // of the call's tool.end
    Str text;
    Str spill;   // the whole report, when the text was cut
    i64 bytes;
};

// A session parked while idle (park): what its log does not hold, for
// its revival.
struct Parked {
    Str sid;
    Str model;
    Str which;     // its endpoint, by name or URL
    iz  lock;      // still held, -1 if none
    b32 attended;
    b32 closed;
};

struct Endpoint {
    Str name;       // its configured section, empty if none
    Str url;
    Str key;        // bearer token (x-api-key for Anthropic), empty if none
    i64 firstbyte;  // watchdogs, in ticks
    i64 idle;
    i64 progress;
    i32 connectms;
    i32 kind;       // EP_*
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
    b32  nojob;     // timed out with too many jobs to move it to one
    i32  tool;      // TOOL_SHELL, or TOOL_GREP or TOOL_GLOB searching
    Str  command;   // the shell's
    Str  root;      // a search's directory, normalized
    Str  pattern;   // a search's, as given
    Str  glob;      // grep's file filter, or glob's pattern
    b32  fold;      // grep ignore_case
};

// A session's desktop helper (deskrun), while it runs.
struct DeskRun {
    Proc    *proc;
    u64      id;       // its records'
    Region  *region;   // what it sent and not yet taken, reserved at its
    Arena    arena;    // first start, and kept for the session's life
    Buf      in;
    b32      hello;    // its hello was answered
    b32      busy;     // a call waits for its reply
    i32      action;   // that call's, DESK_*
    DeskShot shot;     // the latest screenshot's, which points refer to
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
    i64          hiddenat;   // the first ping of reasoning unseen, or 0
    i64          tokens;     // chunks with output: about one token each
    // The turn
    i64          turn;
    i64          steps;
    i64          attempts;
    i32          retries;    // for the current step
    i32          nudges[N_COUNT];
    u64          lastset;    // hash of the last step's calls
    i32          setrepeat;
    u64          seen;       // what the last step's desktop looks saw
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
    Str          callfail;   // why the current call failed, for tool.end
    Str          callfix;    // what was forgiven in its edit (editrepair)
    Str          callimage;  // its screenshot, relative to dir, and its
    i32          callimgw;   // size, and the screenshot's geometry
    i32          callimgh;   // (deskshotstr), for tool.end
    Str          callshot;
    DeskRun      desk;       // the desktop helper
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
    i64          thinkfrom;  // seq of the latest compaction, which leaves
                             // older reasoning bound to what it changed
    Compact      cp;         // with REQ_COMPACT
    i32          overflows;  // this step's prompts rejected as too long
    i64          ovsize;     // tokens of the first, 0 if none
    Overflow     ovinfo;     // the sizes the latest rejection gave
    // Admission at the endpoint
    AdmPoint    *point;      // with a slot
    i32          slot;       // SLOT_*
    Str          slotmodel;
    u64          admid;      // the session's id in queues
    i32          admepoch;   // its lane's epoch when admitted (admshrink)
    i32          qpos;       // its place in line as last told, and the
    i32          qlen;       // line's length then
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
    // Background jobs, and what the agent hears between requests
    Job         *jobs;       // newest first
    i64          njobs;      // numbers given so far
    Slice<i64>   resumed;    // ticks of turns started for jobs, for the cap
    b32          wake;       // a job ended: start a turn if idle
    b32          closed;     // the frontend closed it: no jobs, no resumes
    Slice<Notice> notices;   // delivered at the next request
    // The todo list
    Slice<TodoItem> todos;
    i32          mutations;  // mutating calls since the list changed
    // Snapshots for rewind
    Slice<Snap>  snaps;
    Slice<CallMark> marks;
    i64          nsnap;      // snap files made so far
    // Instruction files
    Str          instr;      // their text in the system prompt, or null
    Slice<InstrStamp> instrfiles;  // every file loaded, lazily too
    Instructions instrpend;  // those loaded at the start, to log
    b32          instrlog;   // that record is still to log
    // Roles and subagents
    Str          role;       // the role's name, empty if none
    Str          rolepr;     // its prompt, in the system prompt
    i32          depth;      // 0 for a session of the user's
    Str          psid;       // a subagent's parent, else empty
    Str          pcall;      // and the call that started it
    Session     *parent;     // the parent, while both are in memory
    b32          intask;     // the parent waits for this one's report
    i64          treqs;      // requests made for the task
    Slice<TaskRun> tasks;    // the subagents of the calls running
    b32          parkme;     // idle long enough: park it (enginethread)
    // Limits a frontend set (new), as an eval does
    i64          stepmax;    // model requests a turn, 0 for STEP_HARD
    b32          projinstr;  // the project's instruction files, not the user's
    b32          nofallback; // never a fallback model: the one asked, or none
    b32          forget;     // closed: drop it from memory once idle
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
    Str         global;   // the user's own instruction file
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
                          // hook, PROSPERO_TEST_MINUTE_MS, shortens it)
    Slice<Parked> parked; // sessions parked, revived by their next command
    i64         cfgwait[8];  // config.get {refresh} ids awaiting fetches
    i32         ncfgwait;
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
        s->qpos = admpos(s->point, s->admid);
        s->qlen = admlen(s->point);
        putintkey(&w.json, "queue_pos", s->qpos);
        putintkey(&w.json, "queue_len", s->qlen);
    } else if (s->state == S_TASK) {
        i64 n = 0;
        for (iz i = 0; i < s->tasks.len; i++) {
            n += !s->tasks[i].done;
        }
        putintkey(&w.json, "tasks", n);  // subagents still at work
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

// Whether a frontend can answer the session's asks: one that has it
// open attended, or has one of its ancestors so, for a subagent's.
static b32 attending(Session *s)
{
    for (i32 n = 0; s && n<=TASK_DEPTH; s = s->parent, n++) {
        if (s->attended) {
            return 1;
        }
    }
    return 0;
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
static i64 deskimagetokens(Session *);
static i64 estimate(Session *s)
{
    return tokensof(s->rate, s->system.len + s->tools.len + s->bytes) +
           deskimagetokens(s);
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

// Log an assistant record. The blocks are in perm.
static void logassistant(Session *s, Arena scratch, Slice<Block> blocks,
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
        } else if (k->type == BLOCK_REDACTED) {
            putstrkey(&w.json, "data", k->text);
        } else {
            putstrkey(&w.json, "text", k->text);
        }
        if (k->sig.len) {
            putstrkey(&w.json, "sig", k->sig);  // for the next request
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
        if (u->written) {
            putintkey(&w.json, "written", u->written);
        }
        if (s->sentbytes) {
            // The prompt's bytes as estimates count them: with in, the
            // calibration a session opened again starts from
            putintkey(&w.json, "bytes", s->sentbytes);
        }
        putend(&w.json);
    }
    putintkey(&w.json, "ms", (os_ticks(s->e->os) - s->sent)/MS);
    ssend(s, &w);
    if (u && u->in && s->sentbytes) {
        // The server's count calibrates the estimates
        s->rate.tokens = u->in;
        s->rate.bytes  = s->sentbytes;
    }
}

// Append an assistant record and message. The blocks are in perm.
static void addassistant(Session *s, Arena scratch, Slice<Block> blocks,
                         Str finishr, Usage *u)
{
    logassistant(s, scratch, blocks, finishr, u);
    Msg m = {};
    m.role   = ROLE_ASSISTANT;
    m.blocks = blocks;
    addmsg(s, m);
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

// Reset the nudge counters on progress: all of them for new input, but
// for tool progress not those about the todo list, which only its own
// progress resets.
static void resetnudges(Session *s, b32 all = 0)
{
    for (i32 i = 0; i < (all ? N_COUNT : N_TODO); i++) {
        s->nudges[i] = 0;
    }
}

// Queue a notice for the agent's next request. The text is copied.
static void notify(Session *s, Str kind, Str text, i64 job)
{
    Notice n = {};
    n.kind = kind;
    n.text = clone(&s->perm, text);
    n.job  = job;
    s->notices = push(&s->perm, s->notices, n);
}

// Give the agent the notices queued since its last request, each a
// message of its own and a durable record, so that a session opened
// again sends the same history.
static void deliver(Session *s, Arena scratch)
{
    for (iz i = 0; i < s->notices.len; i++) {
        Notice *n = &s->notices[i];
        Arena   a = scratch;
        Buf     b(&a, 256 + n->text.len);
        EvOut   w = sbegin(s, &b, EV_NOTICE);
        putstrkey(&w.json, "kind", n->kind);
        if (n->job) {
            putintkey(&w.json, "job", n->job);
        }
        putstrkey(&w.json, "text", n->text);
        ssend(s, &w);

        Msg m = {};
        m.role = ROLE_USER;
        m.text = n->text;
        addmsg(s, m);
    }
    s->notices = {};
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

static i64 scaled(Engine *, i64 ticks);

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
// its T_ADMIT, and those still waiting hear of any change in their places,
// as some went, or left, or joined the line. Then find when time alone
// may admit more, at any endpoint.
static void pump(Engine *e, AdmPoint *p)
{
    i64 now = os_ticks(e->os);
    for (u64 id; (id = admgo(p, &e->perm, now));) {
        Session *s = sessionbyadm(e, id);
        assert(s && s->slot==SLOT_WAIT && s->point==p);
        s->slot     = SLOT_HELD;
        s->admepoch = admlane(p, 0, s->slotmodel)->epoch;
        arm(s, T_ADMIT, 0);
    }
    for (Session *s = e->sessions; s; s = s->next) {
        if (s->point==p && s->slot==SLOT_WAIT && s->state==S_QUEUED &&
                (admpos(p, s->admid)!=s->qpos || admlen(p)!=s->qlen)) {
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
        admdone(s->point, s->slotmodel, os_ticks(e->os), linger,
                scaled(e, ADM_CALM));
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
    print(&b, "\r\nUser-Agent: prospero/" PROSPERO_VERSION "\r\n");
    if (ep->kind == EP_ANTHROPIC) {
        print(&b, "anthropic-version: ");
        print(&b, anversion);
        print(&b, "\r\n");
        if (ep->key.len) {
            print(&b, "x-api-key: ");
            print(&b, ep->key);
            print(&b, "\r\n");
        }
    } else if (ep->key.len) {
        print(&b, "Authorization: Bearer ");
        print(&b, ep->key);
        print(&b, "\r\n");
    }
    return finish(&b);
}

// What to check when a server refuses the key, to follow its error: the
// setting and the variable it names, and whether that is set, never
// the key itself.
static Str keyhint(Engine *e, Arena *a, Endpoint *ep)
{
    Str set = trim(cfgget(e->cfg, "endpoint", ep->name, "key", 0));
    Buf b(a, 128 + ep->name.len + set.len);
    print(&b, " (check the endpoint's key: ");
    if (!ep->name.len) {
        print(&b, "only an [endpoint] section can set one");
    } else {
        print(&b, "[endpoint ");
        print(&b, ep->name);
        if (!set.len) {
            print(&b, "] sets none");
        } else if (startswith(set, "env:")) {
            print(&b, "] key = ");
            print(&b, set);
            print(&b, ", which is ");
            print(&b, ep->key.len ? Str("set")
                                  : Str("not set where the engine runs"));
        } else {
            print(&b, "] key, written out in the file");
        }
    }
    putbyte(&b, ')');
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
        p->probe   = PROBE_NONE;  // ask again at the next request
        p->probeid = 0;
        p->models  = 1;
    }
}

// The limits at an endpoint: as configured, else as learned. Until the
// probe answers, one counted model at a time, and so too while it has
// only failed: no answer is no reason to load models side by side.
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
        } else if (!p->status || p->status==429 || p->status>=500) {
            // No answer (a router still starting, a timeout): one model
            // at a time, and ask again at the next request. Any other
            // status is an answer, from a server that is no router.
            p->probe  = PROBE_NONE;
            p->models = 1;
        } else {
            p->models = 0;
        }
        pointlimits(p);
        pump(e, p);
    } break;
    }
    return 1;
}

static ModelConf *reqmc(Session *s)
{
    return s->purpose==REQ_COMPACT ? &s->cp.mc : &s->mc;
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
    Str       id    = reqmc(s)->name;  // variants of a model share it
    AdmPoint *p     = admpoint(&e->adm, &e->perm, ep->url);
    assert(s->slot == SLOT_NONE);

    // What the endpoint's listing says of the model: the session's own
    // from its discovery, any other, or one its discovery missed, from
    // the last listing seen
    ModelInfo mi = {};
    if (id==s->mc.name && ep->url==s->ep.url && s->info.found) {
        mi = s->info;
    } else {
        i64 *k = upsert(&e->kinds, kindkey(&scratch, ep->url, id), (Arena *)0);
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
    s->slotmodel = id;
    admenqueue(p, &e->perm, s->admid, id, cap, mi.llamacpp || p->cfgmodels,
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
static void beginturn(Session *, Arena, Str via);
static b32  busy(Session *);
static i32  get32(Str);
static i64  get64(Str);
static void starttasks(Session *, Arena);
static void taskcancel(Session *, Arena);
static void taskcheck(Session *, Arena, Str why, Str err);
static b32  taskbudget(Session *, Arena);
static i64  parkafter(Engine *);
static b32  deskrun(Session *, Arena, DeskArgs *);
static Str  deskdenial(Arena *, i32 by, Str note, i64 wait);
static Slice<Msg> deskimages(Session *, Arena *);

// The ids of the session's calls so far, which a new call's must not
// repeat (finishpartial).
static Slice<Str> usedids(Session *s, Arena *a)
{
    Slice<Str> r = {};
    for (iz i = 0; i < s->marks.len; i++) {
        r = push(a, r, s->marks[i].call);
    }
    return r;
}

static void resetattempt(Session *s)
{
    s->attempt = newarena(s->aregion);
}

// A new attempt's response state, in its fresh arena.
static void newattempt(Session *s)
{
    resetattempt(s);
    s->part     = newpartial(&s->attempt);
    s->sse      = newsse(&s->attempt, SSE_MAX);
    s->cycles   = alloc<Cycle>(&s->attempt, 2);
    s->cycleat  = -1;
    s->ctlwhy   = CTL_NONE;
    s->hiddenat = 0;
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
    req.mc        = &s->mc;
    req.system    = s->system;
    req.tools     = s->tools;
    req.history   = deskimages(s, &scratch);
    req.thinkfrom = s->thinkfrom;
    b32 an        = s->ep.kind == EP_ANTHROPIC;
    Str body      = an ? anencode(&s->attempt, scratch, &req)
                       : encodechat(&s->attempt, scratch, &req);
    Str url       = apiurl(&scratch, s->ep.url, an ? Str("messages")
                                                   : Str("chat/completions"));
    s->sentbytes  = s->system.len + s->tools.len + s->bytes;
    s->sentest    = estimate(s);

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
    // Anthropic's listing comes in pages, of at most 1000 models
    Str url = apiurl(&scratch, s->ep.url, s->ep.kind==EP_ANTHROPIC ?
                     Str("models?limit=1000") : Str("models"));
    resetattempt(s);
    s->discovered = 1;
    if (s->ep.kind==EP_ANTHROPIC && s->mc.kwargs.len) {
        Str msg = concat(&scratch, "the template settings of [model ", s->model);
        msg = concat(&scratch, msg, "] are not sent: kind anthropic takes "
                     "no chat_template_kwargs");
        note(s, scratch, "warn", msg);
    }
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
        Str  key     = kindkey(&scratch, s->ep.url, s->mc.name);
        i64 *cached  = upsert(&e->trained, key, (Arena *)0);
        i64  trained = cached ? *cached : 0;
        ModelInfo mi = parsemodels(scratch, finish(&s->body), s->mc.name, trained);
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
            s->mc.maxout = mi.maxout;
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

static void gohome(Session *, Arena, Str why);

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
    if (s->purpose == REQ_CHAT) {
        gohome(s, scratch, "a slow retry");  // the record names the target
    }
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
// starts its cooldown. Returns 0 at the end of the chain, and at once
// for a session that must keep its model (new's fallback, as an eval's).
static b32 nexttarget(Session *s, Arena scratch, i32 cls, i32 status, Str msg)
{
    if (s->nofallback) {
        return 0;
    }
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

// Leave out of requests all the reasoning sent back so far (thinkfrom).
// Returns 0 if there was none to leave out.
static b32 unbind(Session *s)
{
    b32 any = 0;
    for (iz i = 0; i < s->history.len; i++) {
        Msg *m = &s->history[i];
        for (iz k = 0; m->seq>=s->thinkfrom && k<m->blocks.len; k++) {
            any |= m->blocks[k].type==BLOCK_REDACTED || m->blocks[k].sig.len;
        }
    }
    s->thinkfrom = any ? s->log.seq+1 : s->thinkfrom;
    return any;
}

// Fail the current attempt. Transient failures back off and retry while
// nothing visible has streamed, and otherwise commit what streamed and
// ask the agent to continue. Once quick retries are spent, fallbacks and
// slow retries follow (above). retryms is the server's wish, -1 if none.
static void fail(Session *s, Arena scratch, i32 cls, i32 status, Str msg, i64 retryms)
{
    if (s->slot==SLOT_HELD && contains(msg, "Context size has been exceeded")) {
        // llama.cpp's KV pool, shared by its slots, is full: it failed
        // every request in flight, which would fail again together
        Engine *e    = s->e;
        i64     now  = os_ticks(e->os);
        i32     most = admshrink(s->point, s->slotmodel, s->admepoch, now,
                                 scaled(e, ADM_CALM));
        if (most) {
            Buf b(&scratch, 256 + s->slotmodel.len);
            print(&b, "the server's memory for contexts is full: ");
            print(&b, (i64)most);
            print(&b, most==1 ? Str(" request") : Str(" requests"));
            print(&b, " at once to ");
            print(&b, s->slotmodel);
            print(&b, " from now on, and one more for each 5 minutes "
                      "without this error");
            note(s, scratch, "warn", finish(&b));
        }
    }
    if (s->purpose == REQ_COMPACT) {
        failcompact(s, scratch, cls, status, msg, retryms);
        return;
    }
    stopreq(s);
    Partial *p       = s->part;
    b32      visible = p && committed(p);
    s->part = 0;

    if (cls==CLASS_DETERMINISTIC && reqep(s)->kind==EP_ANTHROPIC &&
            contains(msg, "signature") && contains(msg, "thinking") &&
            unbind(s)) {
        // Anthropic's reasoning is bound to the conversation before it,
        // which the API found changed: the step goes again, once,
        // without the reasoning, which removing it all allows
        retryevent(s, scratch, RETRY_QUICK, cls, status, 0, msg);
        request(s, scratch);
        return;
    }

    if ((cls==CLASS_TRANSIENT || cls==CLASS_LOOP) && visible) {
        // Keep what streamed: the cycle cut away, an incomplete call
        // dropped, complete calls run as usual
        if (cls==CLASS_LOOP && s->cycleat>=0) {
            p->text.len = s->cycleat<p->text.len ? s->cycleat : p->text.len;
        }
        Slice<Str> used = usedids(s, &scratch);
        Finished   f    = finishpartial(&s->perm, p, 0, s->turn, s->steps, used);
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
    // Past the end of the chain, a fallback's own limit or absence is no
    // reason to stop: the slow wait starts over from the primary, which
    // the session left perhaps only for a passing outage.
    b32 later = cls==CLASS_TRANSIENT && retryms>RETRY_WAIT/MS;
    b32 gone  = cls==CLASS_DETERMINISTIC && status==404;
    b32 away  = s->target && (cls==CLASS_LIMIT || gone);
    if (cls==CLASS_TRANSIENT && !later &&
            quickretry(s, scratch, cls, status, msg, retryms)) {
        return;
    } else if ((cls==CLASS_TRANSIENT || cls==CLASS_LIMIT || gone) &&
               nexttarget(s, scratch, cls, status, msg)) {
        proceed(s, scratch);
        return;
    } else if ((cls==CLASS_TRANSIENT || away) &&
               slowwait(s, scratch, cls, status, msg, later ? retryms : -1)) {
        return;
    }

    // Terminal: keep any visible text, but no calls, which would need
    // results that will never come
    if (visible) {
        p->calls = {};
        Slice<Str> used = usedids(s, &scratch);
        Finished   f    = finishpartial(&s->perm, p, 1, s->turn, s->steps, used);
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
    Chunk c = reqep(s)->kind==EP_ANTHROPIC ? anchunk(s->part, j, &scratch)
                                           : applychunk(s->part, j, &scratch);
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
    } else if (c.hidden) {
        // Reasoning that streams unseen is progress for as long as the
        // response could take at a slow rate, and no longer
        i64 now  = os_ticks(s->e->os);
        i64 most = anmaxtokens(reqmc(s)) * SEC / AN_SLOWRATE;
        s->hiddenat = s->hiddenat ? s->hiddenat : now;
        if (now-s->hiddenat < most) {
            arm(s, T_PROGRESS, ep->progress);
        }
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
        if (ev->type=="error" && reqep(s)->kind!=EP_ANTHROPIC) {
            Str msg = errortext(jsonget(d.root, "error"));
            fail(s, t, classify(0, msg), 0, msg, -1);
            return;
        }
        if (!onchunk(s, t, d.root)) {  // Anthropic's error events too
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
        if (err == HTTP_ETRUST) {
            // No retry will change it: someone must trust the certificate,
            // or fix the server or the endpoint's URL
            Str msg = contains(text, "certificate") ? text :
                      concat(&scratch, "the server's certificate was refused: ", text);
            fail(s, scratch, CLASS_FATAL, 0, msg, -1);
            return;
        }
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
        Str hint = {};
        if (cls==CLASS_FATAL && (s->status==401 || s->status==403)) {
            hint = keyhint(e, &scratch, reqep(s));
        }
        Buf b(&scratch, 64 + msg.len + hint.len);
        print(&b, "HTTP ");
        print(&b, (i64)s->status);
        if (msg.len) {
            print(&b, ": ");
            print(&b, msg.len>2000 ? utf8head(msg, 2000) : msg);
        }
        print(&b, hint);
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

// Whether a step's calls include a desktop action that only looks.
static b32 desklooking(Session *s, Arena scratch, Slice<Block> calls)
{
    for (iz i = 0; i < calls.len; i++) {
        Arena a = scratch;
        if (toolfind(calls[i].name)==TOOL_DESKTOP && tooloffered(&s->te, TOOL_DESKTOP)) {
            Args args = parseargs(&a, TOOL_DESKTOP, calls[i].args, s->te.edit);
            if (desklooks(args.desk.action)) {
                return 1;
            }
        }
    }
    return 0;
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

// What the harness forgave in a call that ran: its arguments' syntax
// (repairargs), and in an edit what editrepair says. Null if nothing.
static Str callrepair(Arena *a, Block *c, Str fix)
{
    Str args = repairargs(a, c->args).how;
    if (args.len && fix.len) {
        return concat(a, concat(a, args, "; "), fix);
    }
    return args.len ? args : fix;
}

// End the current call. A command moved to the background names its
// job, and a task its subagent's session. Why it failed (callfail) and
// what was forgiven (callrepair) go into the record too, so that an
// eval can count them by kind.
static void toolend(Session *s, Arena scratch, Str status, Str text,
                    i32 code, b32 hascode, Str spill, i64 bytes, i64 job = 0,
                    Str child = {})
{
    Engine *e      = s->e;
    Block  *c      = &s->calls[s->callidx];
    Str     repair = status=="cancelled" ? Str{} : callrepair(&scratch, c, s->callfix);
    Buf     b(&scratch, 512 + text.len);
    EvOut   w = sbegin(s, &b, EV_TOOLEND);
    putstrkey(&w.json, "call", c->id);
    putstrkey(&w.json, "status", status);
    if (hascode) {
        putintkey(&w.json, "exit", code);
    }
    if (s->callfail.len) {
        putstrkey(&w.json, "fail", s->callfail);
    }
    if (repair.len) {
        putstrkey(&w.json, "repair", repair);
    }
    putstrkey(&w.json, "text", text);
    if (spill.len) {
        putstrkey(&w.json, "spill", spill);
        putintkey(&w.json, "bytes", bytes);
    }
    if (job) {
        putintkey(&w.json, "job", job);
    }
    if (child.len) {
        putstrkey(&w.json, "child", child);
    }
    if (s->callimage.len) {
        putstrkey(&w.json, "image", s->callimage);
        putintkey(&w.json, "width", s->callimgw);
        putintkey(&w.json, "height", s->callimgh);
        putstrkey(&w.json, "shot", s->callshot);
    }
    putintkey(&w.json, "ms", (os_ticks(e->os) - s->callstart)/MS);
    ssend(s, &w);

    Msg m = {};
    m.role   = ROLE_TOOL;
    m.callid = c->id;
    m.text   = clone(&s->perm, text);
    m.spill  = spill.len ? clone(&s->perm, spill) : Str{};
    m.error  = toolfailed(status);
    m.image  = s->callimage;
    m.imgw   = s->callimgw;
    m.imgh   = s->callimgh;
    addmsg(s, m);
    s->callimage = {};
    s->callshot  = {};
    b32 done = status=="ok" || status=="background";
    s->progressed |= done;
    if (done && toolmutates(toolfind(c->name))) {
        s->mutations++;
    }
    s->callfail = {};
    s->callfix  = {};
    s->callidx++;
}

static void toolstartat(Session *s, Arena scratch, iz k)
{
    Block *c = &s->calls[k];
    Buf    b(&scratch, 512 + c->args.len);
    EvOut  w = sbegin(s, &b, EV_TOOLSTART);
    putstrkey(&w.json, "call", c->id);
    putstrkey(&w.json, "name", c->name);
    putstrkey(&w.json, "args", c->args);
    putstrkey(&w.json, "cwd", s->cwd);
    ssend(s, &w);
    s->callstart = os_ticks(s->e->os);
    CallMark m = {c->id, s->log.seq, s->turn};
    s->marks = push(&s->perm, s->marks, m);
}

static void toolstart(Session *s, Arena scratch)
{
    toolstartat(s, scratch, s->callidx);
}


// Snapshots and rewind
//
// Before write or edit changes a file, the harness keeps its original as
// snap/<n> in the session directory, or nothing when the file did not
// exist, and once the change is made it logs a snap record with hashes
// of the bytes before and after. A rewind (cmdrewind) puts back, newest
// first, every file changed at or after a call or a turn, but only
// where the file still holds what the harness wrote: anything else has
// changed it since, which is a conflict, left alone unless forced.
// Commands change files too, and those changes are not tracked.

struct SnapPre {
    i64 n;        // 0 if nothing can be undone
    u64 before;   // hash of the original
    b32 existed;
};

static Str hashstr(Arena *a, u64 h)
{
    Buf b(a, 16);
    printhex(&b, h, 16);
    return finish(&b);
}

static Str snapname(Arena *a, i64 n)
{
    Buf b(a, 32);
    print(&b, "snap/");
    print(&b, n);
    return finish(&b);
}

static SnapPre snapbefore(Session *s, Arena scratch, Str path)
{
    SnapPre r  = {};
    Os     *os = s->e->os;
    Read    rd = os_read(os, &scratch, path, (iz)FILE_MAX);
    if (rd.err == READ_NOTFOUND) {
        r.n = ++s->nsnap;
        return r;
    } else if (rd.err) {
        return r;  // a directory, or too big: the tool fails as well
    }
    i64 n    = s->nsnap + 1;
    Str dir  = join(&scratch, s->dir, "snap");
    Str file = join(&scratch, s->dir, snapname(&scratch, n));
    if (!os_mkdirs(os, scratch, dir) || !os_write(os, scratch, file, rd.data)) {
        return r;
    }
    s->nsnap  = n;
    r.n       = n;
    r.before  = hash(rd.data);
    r.existed = 1;
    return r;
}

// After the tool: log the change it made (wrote, or the file is gone),
// or drop the copy.
static void snapafter(Session *s, Arena scratch, Str path, SnapPre *pre, Str wrote, b32 gone)
{
    Os *os   = s->e->os;
    Str name = snapname(&scratch, pre->n);
    if (!pre->n) {
        return;
    } else if (!wrote.data && !gone) {
        if (pre->existed) {
            Str file = join(&scratch, s->dir, name);
            os_remove(os, scratch, file);
        }
        return;
    }
    Block *c      = &s->calls[s->callidx];
    Str    before = pre->existed ? hashstr(&scratch, pre->before) : Str("");
    Str    after  = gone ? Str("") : hashstr(&scratch, hash(wrote));
    {
        Arena a = scratch;
        Buf   b(&a, 512 + path.len);
        EvOut w = sbegin(s, &b, EV_SNAP);
        putstrkey(&w.json, "call", c->id);
        putstrkey(&w.json, "path", path);
        putstrkey(&w.json, "snap", name);
        putintkey(&w.json, "existed", pre->existed);
        putstrkey(&w.json, "hash_before", before);
        putstrkey(&w.json, "hash_after", after);
        ssend(s, &w);
    }
    Snap sn = {};
    sn.n       = pre->n;
    sn.seq     = s->log.seq;
    sn.turn    = s->turn;
    sn.call    = c->id;
    sn.path    = clone(&s->perm, path);
    sn.before  = clone(&s->perm, before);
    sn.after   = clone(&s->perm, after);
    sn.existed = pre->existed;
    s->snaps = push(&s->perm, s->snaps, sn);
}


// Instruction files (instructions.cpp)

static Slice<Str> instrlist(Session *s, Arena *a)
{
    return instrnames(a, cfgget(s->e->cfg, "instructions", "", "files", 0));
}

static void instrstamp(Session *s, Slice<InstrFile> files)
{
    for (iz i = 0; i < files.len; i++) {
        InstrStamp k = {};
        k.path  = clone(&s->perm, files[i].path);
        k.size  = files[i].size;
        k.mtime = files[i].mtime;
        s->instrfiles = push(&s->perm, s->instrfiles, k);
    }
}

// The durable record of instruction files loaded: at the start (why
// "start", with the text that went into the system prompt), for a
// directory below the working directory ("nested"), or after a change
// ("changed").
static void loginstr(Session *s, Arena scratch, Slice<InstrFile> files,
                     Slice<Str> skipped, Str why, Str text)
{
    Buf   b(&scratch, 1024 + text.len);
    EvOut w = sbegin(s, &b, EV_INSTRUCTIONS);
    putstrkey(&w.json, "why", why);
    putkey(&w.json, "files");
    putarray(&w.json);
    for (iz i = 0; i < files.len; i++) {
        putobject(&w.json);
        putstrkey(&w.json, "path", files[i].path);
        putintkey(&w.json, "bytes", files[i].size);
        putintkey(&w.json, "mtime", files[i].mtime);
        if (files[i].importer.data) {
            putstrkey(&w.json, "importer", files[i].importer);
        }
        putend(&w.json);
    }
    putend(&w.json);
    if (skipped.len) {
        putkey(&w.json, "skipped");
        putarray(&w.json);
        for (iz i = 0; i < skipped.len; i++) {
            putstr(&w.json, skipped[i]);
        }
        putend(&w.json);
    }
    if (text.data) {
        putstrkey(&w.json, "text", text);
    }
    ssend(s, &w);
}

// The instructions of directories between the working directory and a
// file a tool worked on, not loaded yet, for the next request.
static void instrlazy(Session *s, Arena scratch, Str path)
{
    Engine    *e      = s->e;
    Slice<Str> loaded = {};
    for (iz i = 0; i < s->instrfiles.len; i++) {
        loaded = push(&scratch, loaded, s->instrfiles[i].path);
    }
    Slice<Str>   names = instrlist(s, &scratch);
    Instructions in    = instrnested(e->os, &scratch, s->guard.cwd, path, names,
                                     loaded, e->home, s->te.windows);
    if (!in.files.len) {
        return;
    }
    Str dir = normpath(&scratch, in.files[0].path, "..", s->te.windows);
    Buf b(&scratch, 256 + in.text.len);
    print(&b, "Instructions for the files under ");
    print(&b, showpath(s->guard.cwd, dir));
    print(&b, " apply from now on. Follow them when you work there:");
    print(&b, in.text);
    Str text = finish(&b);
    loginstr(s, scratch, in.files, in.skipped, "nested", {});
    instrstamp(s, in.files);
    notify(s, "instructions", text, 0);
}

// Loaded instruction files that changed since: their new text, for the
// next request, since the system prompt stays as it was.
static void instrcheck(Session *s, Arena scratch)
{
    Engine          *e       = s->e;
    Os              *os      = e->os;
    Slice<InstrFile> changed = {};
    Str              root    = {};
    Buf              b(&scratch, 1024);
    for (iz i = 0; i < s->instrfiles.len; i++) {
        InstrStamp *k    = &s->instrfiles[i];
        Stat        st   = os_stat(os, scratch, k->path);
        b32         file = st.type == STAT_FILE;
        if (file ? st.size==k->size && st.mtime==k->mtime : k->mtime<0) {
            continue;  // as it was, or gone and told so
        }
        if (file && k->path!=e->global) {
            // A project's file that now leads out of it is gone as one
            b32 win = s->te.windows;
            root = root.data ? root : instrroot(os, &scratch, s->guard.cwd, win);
            if (within(root, k->path, win) &&
                    !instrinside(os, scratch, root, k->path, win)) {
                if (k->mtime < 0) {
                    continue;  // and told so
                }
                st   = {};
                file = 0;
            }
        }
        k->size  = file ? st.size : 0;
        k->mtime = file ? st.mtime : -1;
        InstrFile f = {};
        f.path  = k->path;
        f.size  = st.size;
        f.mtime = st.mtime;
        changed = push(&scratch, changed, f);
        print(&b, "\n\n## ");
        print(&b, k->path);
        Read rd = st.type==STAT_FILE ? os_readat(os, &scratch, k->path, 0, (iz)INSTR_MAX)
                                     : Read{};
        if (st.type!=STAT_FILE || rd.err) {
            print(&b, "\n\nThis file is gone: its instructions no longer apply.");
        } else {
            print(&b, " (new version)\n\n");
            print(&b, trimright(rd.data));
        }
    }
    if (!changed.len) {
        return;
    }
    Str  body = finish(&b);
    Buf  t(&scratch, 256 + body.len);
    print(&t, "Instruction files changed since you read them. Follow them "
              "as they are now:");
    print(&t, body);
    Str text = finish(&t);
    loginstr(s, scratch, changed, {}, "changed", {});
    notify(s, "instructions", text, 0);
}


// Searches (search.cpp) and file tools

static b32 startsearch(Session *, Arena, i32 tool, Str path, Args *);

// The files a patch names, judged by the guard: those outside the roots,
// unless the user allowed one, to be asked about (out), and at most one
// such (many is set).
struct PatchGuard {
    GuardPath out;
    b32       many;
};

static PatchGuard patchguard(Session *s, Arena *a, Patch *p, Str allowed)
{
    PatchGuard r = {};
    for (iz i = 0; i < p->files.len; i++) {
        PatchFile *f = &p->files[i];
        for (i32 k = 0; k < 2; k++) {
            Str want = k ? f->to : f->path;
            if (!want.data) continue;
            GuardPath g = guardpath(s->e->os, a, &s->guard, want, 0);
            if (k) {
                f->fileto = g.path;
            } else {
                f->file = g.path;
            }
            if (g.inside || g.path==allowed || g.path==r.out.path) {
                continue;
            } else if (r.out.path.data) {
                r.many = 1;
            }
            r.out = g;
        }
    }
    return r;
}

// edit as a patch: change, with snapshots, every file the patch names,
// all inside the roots but the one the user may have allowed. Each
// change is worked out before any is made.
static void runpatch(Session *s, Arena scratch, Args *args, Str allowed)
{
    Engine    *e  = s->e;
    Patch     *p  = &args->patch;
    PatchGuard pg = patchguard(s, &scratch, p, allowed);
    if (pg.out.path.data) {
        Str text = concat(&scratch, "Not run: the patch changes a file "
                          "outside the working directory that was not "
                          "allowed: ", pg.out.path);
        toolend(s, scratch, "denied", text, 0, 0, {}, 0);
        return;
    }
    PatchPlan plan = planpatch(e->os, &scratch, s->guard.cwd, p, s->te.windows);
    if (!plan.ok) {
        s->callfail = "miss";
        toolend(s, scratch, "error", plan.text, 0, 0, {}, 0);
        return;
    }
    s->callfix = plan.fix;
    iz made = 0;
    for (; made < plan.changes.len; made++) {
        FileChange *c   = &plan.changes[made];
        SnapPre     pre = snapbefore(s, scratch, c->path);
        b32         ok  = 0;
        if (c->data.data) {
            Str dir = parentdir(c->path);
            ok = (!dir.len || os_stat(e->os, scratch, dir).type==STAT_DIR ||
                  os_mkdirs(e->os, scratch, dir)) &&
                 os_write(e->os, scratch, c->path, c->data);
        } else {
            ok = os_remove(e->os, scratch, c->path);
        }
        snapafter(s, scratch, c->path, &pre, ok ? c->data : Str{}, ok && !c->data.data);
        if (!ok) {
            break;
        }
        instrlazy(s, scratch, c->path);
    }
    if (made < plan.changes.len) {
        Buf b(&scratch, 512);
        print(&b, "The patch was applied only in part: ");
        print(&b, showpath(s->guard.cwd, plan.changes[made].path));
        print(&b, plan.changes[made].data.data ? Str(" could not be written")
                                               : Str(" could not be removed"));
        print(&b, made ? Str(", after these were changed: ") : Str(", and nothing else was changed."));
        for (iz i = 0; i < made; i++) {
            print(&b, i ? Str(", ") : Str(""));
            print(&b, showpath(s->guard.cwd, plan.changes[i].path));
        }
        print(&b, made ? Str(". Read them before you go on.") : Str(""));
        toolend(s, scratch, "error", finish(&b), 0, 0, {}, 0);
        return;
    }
    toolend(s, scratch, "ok", plan.text, 0, 0, {}, 0);
}

// A path tool on a path the guard allowed, or the user did. Returns 1
// if the call finished, 0 if it goes on (a search).
static b32 runfile(Session *s, Arena scratch, i32 tool, Str path, Args *args)
{
    if (tool==TOOL_GREP || tool==TOOL_GLOB) {
        return startsearch(s, scratch, tool, path, args);
    }
    Engine *e    = s->e;
    ToolOut o    = {};
    Str     ncwd = s->guard.cwd;  // as normalized paths
    SnapPre pre  = {};
    b32     hl   = s->te.edit == EDIT_HASHLINE;
    if (tool==TOOL_EDIT && s->te.edit==EDIT_PATCH) {
        runpatch(s, scratch, args, path);
        return 1;
    }
    if (tool==TOOL_WRITE || tool==TOOL_EDIT) {
        pre = snapbefore(s, scratch, path);
    }
    switch (tool) {
    case TOOL_READ:  o = toolread(e->os, &scratch, ncwd, path, args, hl); break;
    case TOOL_WRITE: o = toolwrite(e->os, &scratch, ncwd, path, args);    break;
    case TOOL_EDIT:  o = hl ? toolhash(e->os, &scratch, ncwd, path, args)
                            : tooledit(e->os, &scratch, ncwd, path, args);
                     break;
    }
    if (tool==TOOL_WRITE || tool==TOOL_EDIT) {
        snapafter(s, scratch, path, &pre, o.wrote, 0);
    }
    if (o.ok) {
        instrlazy(s, scratch, path);
    }
    s->callfail = tool==TOOL_EDIT && !o.ok ? Str("miss") : Str{};
    s->callfix  = o.fix;
    toolend(s, scratch, o.ok ? Str("ok") : Str("error"), o.text, 0, 0, {}, 0);
    return 1;
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
    if (allow && k->tool==TOOL_DESKTOP) {
        Args args = parseargs(&scratch, k->tool, c->args, s->te.edit);
        deskrun(s, scratch, &args.desk);  // which may go on
        return;
    } else if (allow) {
        Args args = parseargs(&scratch, k->tool, c->args, s->te.edit);
        runfile(s, scratch, k->tool, k->gp.path, &args);
        return;
    } else if (k->tool == TOOL_DESKTOP) {
        Str text = deskdenial(&scratch, by, note, (t - k->opened + SEC/2) / SEC);
        toolend(s, scratch, by==DENY_CANCEL ? Str("cancelled") : Str("denied"),
                text, 0, 0, {}, 0);
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
    b32       read = toolreads(tool);
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
    b32 wait  = attending(s) && !steer;
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

// out/<call>.log for a call's output, unless a model reused the call id
// in this session: the absolute path, and in rel the path relative to
// the session directory, both in the session's arena.
static Str spillfile(Session *s, Arena scratch, Str id, Str *rel, Str ext = ".log")
{
    for (i32 n = 1;; n++) {
        Arena a = scratch;
        Buf   b(&a, 128);
        print(&b, "out/");
        print(&b, id);
        if (n > 1) {
            putbyte(&b, '-');
            print(&b, (i64)n);
        }
        print(&b, ext);
        Str r    = finish(&b);
        Str path = join(&a, s->dir, r);
        if (os_stat(s->e->os, a, path).type == STAT_MISSING) {
            *rel = clone(&s->perm, r);
            return clone(&s->perm, path);
        }
    }
}

// Spawn a process for the current call, its output into spill.
static Proc *spawncall(Session *s, Arena scratch, Slice<Str> argv, Str cmdline,
                       Str cwd, Str spill, i64 maxbytes, u64 *id)
{
    Engine    *e       = s->e;
    Slice<Str> secrets = cfgsecrets(e->cfg, &scratch);
    Spawn      sp      = {};
    sp.argv     = argv;
    sp.cmdline  = cmdline;
    sp.cwd      = cwd;
    sp.env      = shellenv(&scratch, e->env, s->te.windows, secrets);
    sp.outpath  = spill;
    sp.maxbytes = maxbytes;
    sp.mbox     = e->in;
    sp.id       = *id = ++e->nextid;
    Proc *p = os_spawn(e->os, &sp, scratch);
    if (!p) {
        *id = 0;
    }
    return p;
}

// A duration for people: "45 s", "3 min 12 s", "2 h 5 min".
static void printelapsed(Buf *b, i64 ticks)
{
    i64 secs = (ticks + SEC/2) / SEC;
    if (secs < 60) {
        print(b, secs);
        print(b, " s");
    } else if (secs < 3600) {
        print(b, secs/60);
        print(b, " min ");
        print(b, secs%60);
        print(b, " s");
    } else {
        print(b, secs/3600);
        print(b, " h ");
        print(b, secs/60%60);
        print(b, " min");
    }
}


// Background jobs
//
// A job is a command left running: one still running at its timeout,
// or one started with background. It keeps its process and its output
// file while the session goes on. It runs until it exits, or until its
// deadline (the [tools] job_timeout unless the agent asked for another,
// up to job_timeout_max), when it is stopped as a cancelled command is:
// asked to stop (os_kill soft), then killed when the grace period is
// over ([tools] stop_grace). Its end is logged (job.end) and told to
// the agent at its next request, as a notice; a session that is idle
// then starts a turn for it by itself, at most [tools] auto_resume
// times an hour. Closing a session stops its jobs; quitting kills them.
// A cancelled command is stopped the same way, as a hidden job.

static i64 stopgrace(Engine *e)
{
    if (trim(cfgget(e->cfg, "tools", "", "stop_grace", 0)).len) {
        return cfgms(e->cfg, "tools", "", "stop_grace", 0) * MS;
    }
    return HOST_WINDOWS ? 3*SEC : 2*SEC;
}

static Job *jobfind(Session *s, i64 n)
{
    for (Job *j = s->jobs; j; j = j->next) {
        if (j->n==n && !j->hidden) {
            return j;
        }
    }
    return 0;
}

static i32 jobsrunning(Session *s)
{
    i32 n = 0;
    for (Job *j = s->jobs; j; j = j->next) {
        n += j->state!=JOB_DONE && !j->hidden;
    }
    return n;
}

// Arm T_JOBS for the nearest of the jobs' deadlines.
static void jobarm(Session *s)
{
    i64 at = 0;
    for (Job *j = s->jobs; j; j = j->next) {
        i64 t = j->state==JOB_DONE ? 0          :
                j->drainat         ? j->drainat :
                j->state==JOB_STOP ? j->killat  :
                                     j->deadline;
        if (t && (!at || t<at)) {
            at = t;
        }
    }
    s->timers[T_JOBS] = at;
}

static Job *newjob(Session *s, Arena scratch, Str call, Str command, Proc *proc,
                   u64 procid, Str spill, Str rel, i64 start, i64 timeout, b32 hidden)
{
    Os  *os = s->e->os;
    Job *j  = alloc<Job>(&s->perm);
    j->n        = hidden ? 0 : ++s->njobs;
    j->call     = clone(&s->perm, call);
    j->command  = clone(&s->perm, command);
    j->spill    = spill;
    j->rel      = rel;
    j->proc     = proc;
    j->procid   = procid;
    j->start    = start;
    j->deadline = start + timeout*SEC;
    j->hidden   = hidden;
    j->next     = s->jobs;
    s->jobs     = j;
    if (!hidden) {
        i64   now = os_ticks(os);
        Buf   b(&scratch, 512 + command.len);
        EvOut w = sbegin(s, &b, EV_JOBSTART);
        putintkey(&w.json, "job", j->n);
        putstrkey(&w.json, "call", call);
        putstrkey(&w.json, "command", command);
        putstrkey(&w.json, "spill", rel);
        putintkey(&w.json, "deadline", os_time(os) + (j->deadline - now)/MS);
        ssend(s, &w);
    }
    jobarm(s);
    return j;
}

// Ask a running job to stop, and kill it when the grace period is over.
static void jobstop(Session *s, Job *j, i32 why)
{
    if (j->state != JOB_RUN) {
        return;
    }
    os_kill(s->e->os, j->proc, 0);
    j->state  = JOB_STOP;
    j->why    = why;
    j->killat = os_ticks(s->e->os) + stopgrace(s->e);
    jobarm(s);
}

static b32 canresume(Session *s)
{
    Engine *e   = s->e;
    i64     cap = parseint(trim(cfgget(e->cfg, "tools", "", "auto_resume", 0))).val;
    i64     now = os_ticks(e->os);
    i64     n   = 0;
    for (iz i = 0; i < s->resumed.len; i++) {
        n += now - s->resumed[i] < 3600*SEC;
    }
    return !e->quitting && !s->readonly && !s->closed && n<cap;
}

// Start a turn to give the agent its notices, a session left idle.
static void autoresume(Session *s, Arena scratch)
{
    s->resumed = push(&s->perm, s->resumed, os_ticks(s->e->os));
    s->wake    = 0;
    beginturn(s, scratch, "job");
}

static Str jobnotice(Session *s, Arena *a, Job *j, i64 bytes)
{
    Os *os = s->e->os;
    Buf b(a, 1024);
    print(&b, "Background job ");
    print(&b, j->n);
    print(&b, " ended: ");
    switch (j->why) {
    case JOBEND_EXIT:
        if (j->signal) {
            print(&b, "it was killed by signal ");
            print(&b, (i64)j->signal);
        } else {
            print(&b, "it exited with code ");
            print(&b, (i64)j->code);
        }
        break;
    case JOBEND_TIMEOUT:
        print(&b, "it was stopped at its time limit");
        break;
    case JOBEND_KILLED:
        print(&b, "the user stopped it");
        break;
    case JOBEND_CLOSED:
        print(&b, "the session closed");
        break;
    case JOBEND_LIMIT:
        print(&b, "it was killed for writing over 1 GiB of output");
        break;
    case JOBEND_LOST:
        print(&b, "the harness stopped while it ran, which stopped it too");
        break;
    }
    if (j->ran) {  // unknown for a job lost in a crash
        print(&b, ", after ");
        printelapsed(&b, j->ran);
    }
    print(&b, ".\nCommand: ");
    print(&b, j->command);
    print(&b, "\nOutput: ");
    print(&b, bytes);
    print(&b, " bytes, in ");
    print(&b, j->rel);
    Str tail = jobtail(os, a, j->spill);
    if (tail.len) {
        print(&b, "; job_output reads more. It ends with:\n");
        print(&b, tail);
        if (tail[tail.len-1] != '\n') putbyte(&b, '\n');
    } else {
        print(&b, ".\n");
    }
    if (j->cut) {
        print(&b, "[processes it started kept its output open, so they "
                  "were killed]\n");
    }
    return finish(&b);
}

// A job is over: kill what it left, log its end, and tell the agent.
static void jobend(Session *s, Arena scratch, Job *j)
{
    Os *os = s->e->os;
    if (j->proc) {
        os_kill(os, j->proc, 1);
        os_procfree(os, j->proc);
        j->proc = 0;
    }
    j->procid  = 0;
    j->state   = JOB_DONE;
    j->drainat = 0;
    j->ran     = os_ticks(os) - j->start;
    jobarm(s);
    if (j->hidden) {
        for (Job **p = &s->jobs; *p; p = &(*p)->next) {
            if (*p == j) {
                *p = j->next;
                break;
            }
        }
        return;
    }

    Stat st     = os_stat(os, scratch, j->spill);
    i64  bytes  = st.size>j->total ? st.size : j->total;
    b32  tell   = !j->quiet;
    b32  resume = tell && !busy(s) && canresume(s);
    {
        Arena a = scratch;
        Buf   b(&a, 512);
        EvOut w = sbegin(s, &b, EV_JOBEND);
        putintkey(&w.json, "job", j->n);
        putstrkey(&w.json, "call", j->call);
        putstrkey(&w.json, "status", jobendnames[j->why]);
        if (j->why==JOBEND_EXIT && !j->signal) {
            putintkey(&w.json, "exit", j->code);
        } else if (j->signal) {
            putintkey(&w.json, "signal", j->signal);
        }
        putintkey(&w.json, "bytes", bytes);
        putstrkey(&w.json, "spill", j->rel);
        putintkey(&w.json, "ms", j->ran/MS);
        putintkey(&w.json, "notice", tell);
        putintkey(&w.json, "resume", resume);
        ssend(s, &w);
    }
    if (!tell) {
        // A subagent waiting for its jobs still reports once none runs:
        // its last one killed, or all stopped as its session closed
        taskcheck(s, scratch, "done", {});
        return;
    }
    Str text = jobnotice(s, &scratch, j, bytes);
    notify(s, "job", text, j->n);
    s->wake = 1;
    if (resume) {
        autoresume(s, scratch);
    } else if (!busy(s) && !s->closed && !s->e->quitting) {
        note(s, scratch, "info", "a background job ended while this "
             "session already resumed by itself as often as allowed "
             "([tools] auto_resume); the agent hears of it at the next prompt");
    }
    taskcheck(s, scratch, "done", {});  // a subagent waiting for its jobs
}

static void jobrecord(Session *s, Arena scratch, Job *j, Record r)
{
    Os *os = s->e->os;
    switch (r.type) {
    case PROC_TAIL:
        if (!j->hidden) {
            Str   tail = cleanoutput(&scratch, utf8tail(cuthead(r.data, 8), 4096));
            Buf   b(&scratch, 256 + tail.len);
            EvOut w = sbegin(s, &b, EV_JOBOUT);
            putintkey(&w.json, "job", j->n);
            putstrkey(&w.json, "call", j->call);
            putstrkey(&w.json, "tail", tail);
            putintkey(&w.json, "bytes", get64(r.data));
            ssend(s, &w);
        }
        return;
    case PROC_LIMIT:
        if (j->state == JOB_RUN) {
            j->why = JOBEND_LIMIT;
        }
        j->state   = JOB_STOP;
        j->killat  = 0;
        j->drainat = os_ticks(os) + DRAIN;
        os_kill(os, j->proc, 1);
        jobarm(s);
        return;
    case PROC_EXIT:
        j->exited = 1;
        j->code   = get32(r.data);
        j->signal = get32(cuthead(r.data, 4));
        if (j->eof) {
            jobend(s, scratch, j);
        } else if (!j->drainat) {
            j->drainat = os_ticks(os) + DRAIN;
            jobarm(s);
        }
        return;
    case PROC_EOF:
        j->eof   = 1;
        j->total = get64(r.data);
        if (j->exited) {
            jobend(s, scratch, j);
        }
        return;
    }
}

// T_JOBS: deadlines, kills after the grace period, and drains given up.
static void jobtick(Session *s, Arena scratch)
{
    i64 now = os_ticks(s->e->os);
    for (Job *j = s->jobs, *next = 0; j; j = next) {
        next = j->next;
        if (j->state == JOB_DONE) {
            continue;
        } else if (j->drainat && now>=j->drainat) {
            j->cut = j->exited && !j->eof;
            jobend(s, scratch, j);
        } else if (j->state==JOB_STOP && j->killat && now>=j->killat) {
            os_kill(s->e->os, j->proc, 1);
            j->killat  = 0;
            j->drainat = now + DRAIN;
        } else if (j->state==JOB_RUN && now>=j->deadline) {
            jobstop(s, j, JOBEND_TIMEOUT);
        }
    }
    jobarm(s);
}

// Stop every job: asking first, as for a closed session, or at once,
// as when the engine quits.
static void stopjobs(Session *s, Arena scratch, b32 hard)
{
    for (Job *j = s->jobs, *next = 0; j; j = next) {
        next = j->next;
        if (j->state == JOB_DONE) {
            continue;
        }
        j->quiet = 1;  // nobody to tell
        if (j->state == JOB_RUN) {
            j->why = JOBEND_CLOSED;
        }
        if (hard) {
            jobend(s, scratch, j);
        } else {
            jobstop(s, j, JOBEND_CLOSED);
        }
    }
}

static Session *jobbyproc(Engine *e, u64 id, Job **job)
{
    for (Session *s = e->sessions; id && s; s = s->next) {
        for (Job *j = s->jobs; j; j = j->next) {
            if (j->procid == id) {
                *job = j;
                return s;
            }
        }
    }
    return 0;
}

// shell with background: start a job, and end the call at once.
static void startjob(Session *s, Arena scratch, Args *args)
{
    Engine *e = s->e;
    Block  *c = &s->calls[s->callidx];
    if (jobsrunning(s) >= JOB_RUNNING) {
        toolend(s, scratch, "error", "Not started: 16 background jobs are "
                "running already. Stop jobs you no longer need with "
                "job_kill, or wait to be told that they ended.", 0, 0, {}, 0);
        return;
    }
    i64 timeout = s->te.jobtimeout;
    if (args->timeout > 0) {
        timeout = args->timeout<s->te.jobmax ? args->timeout : s->te.jobmax;
    }
    Str      rel   = {};
    Str      spill = spillfile(s, scratch, c->id, &rel);
    ShellCmd sc    = shellcmd(&scratch, &s->te, args->command);
    u64      id    = 0;
    Proc    *p     = spawncall(s, scratch, sc.argv, sc.cmdline, s->cwd, spill,
                               SHELL_OUTMAX, &id);
    if (!p) {
        Str err  = os_lasterr(e->os, &scratch);
        Str text = concat(&scratch, "The shell could not be started: ", err);
        toolend(s, scratch, "error", text, 0, 0, {}, 0);
        return;
    }
    Job *j = newjob(s, scratch, c->id, args->command, p, id, spill, rel,
                    os_ticks(e->os), timeout, 0);
    Buf b(&scratch, 512);
    print(&b, "Started background job ");
    print(&b, j->n);
    print(&b, ". It may run for ");
    printduration(&b, timeout);
    print(&b, ". You will be told when it ends, with the end of its "
              "output: do not poll it or wait for it, but go on with other "
              "work, or end your turn if there is nothing else to do. Use "
              "job_output to read its output so far, and job_kill to stop "
              "it.");
    Str text = finish(&b);
    toolend(s, scratch, "ok", text, 0, 0, rel, 0, j->n);
}

// A job made of the current command takes over what is known of its
// output and its exit, so that it ends as the command would have.
static void jobadopt(Session *s, Job *j)
{
    j->eof    = s->sh.eof;
    j->total  = s->sh.total;
    j->exited = s->sh.exited;
    j->code   = s->sh.code;
    j->signal = s->sh.signal;
    if (s->sh.limited && j->state==JOB_RUN) {
        // Killed already for its output, as jobrecord has it
        j->why     = JOBEND_LIMIT;
        j->state   = JOB_STOP;
        j->drainat = os_ticks(s->e->os) + DRAIN;
        jobarm(s);
    }
}

// A foreground command still running at its timeout becomes a job.
static void movetojob(Session *s, Arena scratch)
{
    Engine *e = s->e;
    Block  *c = &s->calls[s->callidx];
    disarm(s, T_TOOL);
    disarm(s, T_KILL);
    disarm(s, T_DRAIN);
    Job *j = newjob(s, scratch, c->id, s->sh.command, s->proc, s->procid,
                    s->sh.spill, s->sh.rel, s->sh.start, s->te.jobtimeout, 0);
    jobadopt(s, j);
    s->proc   = 0;
    s->procid = 0;
    setstate(s, S_IDLE);

    ShellEnd end = {};
    end.job     = j->n;
    end.timeout = s->sh.timeout;
    ToolOut o  = shellresult(e->os, &scratch, s->sh.spill, &end);
    Stat    st = os_stat(e->os, scratch, s->sh.spill);
    toolend(s, scratch, "background", o.text, 0, 0, s->sh.rel, st.size, j->n);
}

static void tooljobs(Session *s, Arena scratch)
{
    Os *os  = s->e->os;
    i64 now = os_ticks(os);
    Buf b(&scratch, 1024);
    Slice<Job *> list = {};
    for (Job *j = s->jobs; j; j = j->next) {
        if (!j->hidden) {
            list = push(&scratch, list, j);
        }
    }
    if (!list.len) {
        toolend(s, scratch, "ok", "No background jobs have run in this "
                "session.", 0, 0, {}, 0);
        return;
    }
    print(&b, "Background jobs, oldest first:\n");
    for (iz i = list.len-1; i >= 0; i--) {
        Job *j = list[i];
        Stat st = os_stat(os, scratch, j->spill);
        print(&b, "- job ");
        print(&b, j->n);
        print(&b, ": ");
        switch (j->state) {
        case JOB_RUN:
            print(&b, "running for ");
            printelapsed(&b, now - j->start);
            print(&b, " (stopped at its limit in ");
            printelapsed(&b, j->deadline>now ? j->deadline-now : 0);
            putbyte(&b, ')');
            break;
        case JOB_STOP:
            print(&b, "stopping");
            break;
        case JOB_DONE:
            if (j->why==JOBEND_EXIT && !j->signal) {
                print(&b, "exited with code ");
                print(&b, (i64)j->code);
            } else {
                print(&b, "ended (");
                print(&b, jobendnames[j->why]);
                putbyte(&b, ')');
            }
            break;
        }
        print(&b, ", ");
        print(&b, st.size);
        print(&b, " bytes of output: ");
        print(&b, j->command.len>200 ? utf8head(j->command, 200) : j->command);
        putbyte(&b, '\n');
    }
    print(&b, "You are told when each job ends: there is no need to check.");
    Str text = finish(&b);
    toolend(s, scratch, "ok", text, 0, 0, {}, 0);
}

static Str nosuchjob(Arena *a, Session *s, i64 n)
{
    Buf b(a, 128);
    print(&b, "There is no job ");
    print(&b, n);
    print(&b, s->njobs ? Str(": call jobs to list them.")
                       : Str(": no background jobs have run in this session."));
    return finish(&b);
}

static void tooljobout(Session *s, Arena scratch, Args *args)
{
    Os  *os = s->e->os;
    Job *j  = jobfind(s, args->job);
    if (!j) {
        Str text = nosuchjob(&scratch, s, args->job);
        toolend(s, scratch, "error", text, 0, 0, {}, 0);
        return;
    }
    i64      n    = args->lines ? args->lines : JOBOUT_LINES;
    i32      mode = args->offset   ? JOBLINES_AT   :
                    args->from=="head" ? JOBLINES_HEAD : JOBLINES_TAIL;
    JobLines jl   = joblines(os, &scratch, j->spill, mode, n, args->offset);
    Stat     st   = os_stat(os, scratch, j->spill);
    Buf      b(&scratch, 512 + jl.text.len);
    print(&b, "Job ");
    print(&b, j->n);
    print(&b, j->state==JOB_DONE ? Str(" (ended)") : Str(" (still running)"));
    print(&b, ", ");
    print(&b, st.size);
    print(&b, " bytes of output in ");
    print(&b, j->rel);
    if (!jl.count) {
        print(&b, jl.before ? Str(": nothing at that line, past the end.\n")
                            : Str(": no output yet.\n"));
    } else if (mode == JOBLINES_TAIL) {
        print(&b, jl.before ? Str(", its last lines:\n") : Str(", all of it:\n"));
    } else {
        print(&b, ", lines ");
        print(&b, jl.first);
        putbyte(&b, '-');
        print(&b, jl.first + jl.count - 1);
        print(&b, ":\n");
    }
    print(&b, jl.text);
    if (jl.count && jl.after && mode!=JOBLINES_TAIL) {
        print(&b, "[more follows: call job_output with offset=");
        print(&b, jl.first + jl.count);
        print(&b, "]\n");
    }
    if (j->state != JOB_DONE) {
        print(&b, "[you are told when the job ends: do not poll it]");
    }
    Str text = finish(&b);
    toolend(s, scratch, "ok", text, 0, 0, {}, 0);
}

static void tooljobkill(Session *s, Arena scratch, Args *args)
{
    Job *j = jobfind(s, args->job);
    if (!j) {
        Str text = nosuchjob(&scratch, s, args->job);
        toolend(s, scratch, "error", text, 0, 0, {}, 0);
        return;
    }
    Buf b(&scratch, 256);
    print(&b, "Job ");
    print(&b, j->n);
    if (j->state == JOB_DONE) {
        print(&b, " had already ended.");
    } else if (j->state == JOB_STOP) {
        print(&b, " is stopping already.");
        j->quiet = 1;
    } else {
        j->quiet = 1;
        jobstop(s, j, JOBEND_KILLED);
        print(&b, " was asked to stop, and is killed in a few seconds if it "
                  "has not. Its end will not be reported again.");
    }
    Str text = finish(&b);
    toolend(s, scratch, "ok", text, 0, 0, {}, 0);
}


// Commands and searches in the foreground

static b32 startshell(Session *s, Arena scratch, Args *args)
{
    Engine  *e  = s->e;
    Block   *c  = &s->calls[s->callidx];
    ShellRun sh = {};
    sh.tool    = TOOL_SHELL;
    sh.timeout = args->timeout>0 ? args->timeout : SHELL_DEFAULT;
    sh.start   = os_ticks(e->os);
    sh.command = clone(&s->perm, args->command);
    sh.spill   = spillfile(s, scratch, c->id, &sh.rel);
    s->sh = sh;

    ShellCmd sc = shellcmd(&scratch, &s->te, args->command);
    s->proc = spawncall(s, scratch, sc.argv, sc.cmdline, s->cwd, s->sh.spill,
                        SHELL_OUTMAX, &s->procid);
    if (!s->proc) {
        return 0;
    }
    arm(s, T_TOOL, s->sh.timeout*SEC);
    setstate(s, S_TOOL);
    return 1;
}

// grep or glob on a path the guard allowed. Returns 1 if the call
// finished (it could not start), 0 if the search runs.
static b32 startsearch(Session *s, Arena scratch, i32 tool, Str path, Args *args)
{
    Engine *e      = s->e;
    Stat    st     = os_stat(e->os, scratch, path);
    Str     root   = path;
    Str     target = ".";
    if (st.type == STAT_MISSING) {
        Str text = notfound(e->os, &scratch, s->guard.cwd, path);
        toolend(s, scratch, "error", text, 0, 0, {}, 0);
        return 1;
    } else if (st.type==STAT_FILE && tool==TOOL_GLOB) {
        toolend(s, scratch, "error", "That path is a file, but glob searches "
                "a directory: give the directory, or read the file.",
                0, 0, {}, 0);
        return 1;
    } else if (st.type == STAT_FILE) {
        root   = normpath(&scratch, path, "..", s->te.windows);
        target = basename(path);
    } else if (st.type != STAT_DIR) {
        toolend(s, scratch, "error", "That path is neither a file nor a "
                "directory, so it cannot be searched.", 0, 0, {}, 0);
        return 1;
    }

    Block   *c  = &s->calls[s->callidx];
    ShellRun sh = {};
    sh.tool    = tool;
    sh.timeout = SEARCH_TIMEOUT;
    sh.start   = os_ticks(e->os);
    sh.root    = clone(&s->perm, root);
    sh.pattern = clone(&s->perm, args->pattern);
    sh.glob    = clone(&s->perm, tool==TOOL_GLOB ? args->pattern : args->glob);
    sh.fold    = args->ignorecase;
    sh.spill   = spillfile(s, scratch, c->id, &sh.rel);
    s->sh = sh;

    Slice<Str> argv = searchargv(&scratch, &s->te, tool, path, target, args);
    i64        max  = tool==TOOL_GLOB ? SEARCH_GLOBMAX : SEARCH_OUTMAX;
    s->proc = spawncall(s, scratch, argv, {}, root, s->sh.spill, max, &s->procid);
    if (!s->proc) {
        Str err  = os_lasterr(e->os, &scratch);
        Str text = concat(&scratch, "The search could not be started: ", err);
        toolend(s, scratch, "error", text, 0, 0, {}, 0);
        return 1;
    }
    arm(s, T_TOOL, s->sh.timeout*SEC);
    setstate(s, S_TOOL);
    return 0;
}

static void finishshell(Session *s, Arena scratch, b32 cancelled)
{
    Engine *e = s->e;
    disarm(s, T_TOOL);
    disarm(s, T_KILL);
    disarm(s, T_DRAIN);
    if (s->proc && cancelled && !e->quitting) {
        // Stopped as a job is, asked first, while the turn goes on
        Job *j = newjob(s, scratch, {}, {}, s->proc, s->procid, s->sh.spill,
                        s->sh.rel, os_ticks(e->os), 0, 1);
        jobadopt(s, j);
        jobstop(s, j, JOBEND_KILLED);
    } else if (s->proc) {
        os_kill(e->os, s->proc, 1);  // whatever it left behind
        os_procfree(e->os, s->proc);
    }
    s->proc   = 0;
    s->procid = 0;
    setstate(s, S_IDLE);

    Stat st = os_stat(e->os, scratch, s->sh.spill);
    i64  n  = st.size>s->sh.total ? st.size : s->sh.total;
    if (s->sh.tool != TOOL_SHELL) {
        SearchEnd se = {};
        se.tool       = s->sh.tool;
        se.code       = s->sh.code;
        se.signaled   = s->sh.signal || s->sh.code<0;
        se.limited    = s->sh.limited;
        se.timedout   = s->sh.timedout;
        se.cancelled  = cancelled;
        se.root       = s->sh.root;
        se.cwd        = s->guard.cwd;
        se.pattern    = s->sh.pattern;
        se.glob       = s->sh.glob;
        se.ignorecase = s->sh.fold;
        se.windows    = s->te.windows;
        se.tagged     = s->te.edit == EDIT_HASHLINE;
        ToolOut o = searchresult(e->os, &scratch, s->sh.spill, &se);
        Str status = cancelled ? Str("cancelled") : o.ok ? Str("ok") : Str("error");
        toolend(s, scratch, status, o.text, 0, 0, s->sh.rel, n);
        return;
    }

    ShellEnd end  = {};
    end.code      = s->sh.code;
    end.signal    = s->sh.signal;
    end.total     = s->sh.total;
    end.timedout  = s->sh.timedout;
    end.limited   = s->sh.limited;
    end.cut       = s->sh.exited && !s->sh.eof && !cancelled;
    end.cancelled = cancelled;
    end.timeout   = s->sh.timeout;
    end.nojob     = s->sh.nojob;
    ToolOut o = shellresult(e->os, &scratch, s->sh.spill, &end);
    Str status = cancelled    ? Str("cancelled") :
                 end.timedout ? Str("timeout")   :
                 o.ok         ? Str("ok")        : Str("error");
    toolend(s, scratch, status, o.text, end.code, !cancelled && !end.timedout,
            s->sh.rel, n);
}


// todo_write, and the nudges that keep a list honest

static void tooltodo(Session *s, Arena scratch, Args *args)
{
    Block          *c     = &s->calls[s->callidx];
    Slice<TodoItem> items = {};
    i32             active = 0;
    for (iz i = 0; i < args->todos.len; i++) {
        TodoItem t = args->todos[i];
        t.content = clone(&s->perm, t.content);
        items  = push(&s->perm, items, t);
        active += t.status == TODO_ACTIVE;
    }
    if (todoprogress(s->todos, items)) {
        s->nudges[N_TODO] = 0;
    }
    s->todos     = items;
    s->mutations = 0;
    {
        Arena a = scratch;
        Buf   b(&a, 1024);
        EvOut w = sbegin(s, &b, EV_TODO);
        putstrkey(&w.json, "call", c->id);
        putkey(&w.json, "items");
        putarray(&w.json);
        for (iz i = 0; i < items.len; i++) {
            putobject(&w.json);
            putstrkey(&w.json, "content", items[i].content);
            putstrkey(&w.json, "status", todonames[items[i].status]);
            putend(&w.json);
        }
        putend(&w.json);
        ssend(s, &w);
    }
    Str text = todoshow(&scratch, items);
    if (active > 1) {
        text = concat(&scratch, text, "Note: more than one item is in "
                      "progress. Finish one at a time, marking each done as "
                      "soon as it is.");
    }
    toolend(s, scratch, "ok", text, 0, 0, {}, 0);
}

// At the end of a turn: items left undone are a reason to go on. Returns
// 1 if the agent was nudged, and the next step began. An agent waiting
// for a background job is not: the job's end brings it back.
static b32 todonudge(Session *s, Arena scratch)
{
    Str left = todoleft(&scratch, s->todos);
    if (!left.data || (jobsrunning(s) && canresume(s))) {
        return 0;
    }
    Buf b(&scratch, 256 + left.len);
    print(&b, "Your todo list still has items that are not done:\n");
    print(&b, left);
    print(&b, "Continue with them now. If some are finished already, or "
              "will not be done, update the list with todo_write first, "
              "then end your turn.");
    Str text = finish(&b);
    if (!nudge(s, scratch, N_TODO, text)) {
        return 0;
    }
    prepare(s, scratch);
    return 1;
}

// After a step: many changes without the list changing is a sign that
// it has gone stale.
static void todoremind(Session *s, Arena scratch)
{
    if (!s->todos.len || s->mutations<TODO_REMIND) {
        return;
    }
    s->mutations = 0;
    nudge(s, scratch, N_REMIND, "You have made 12 changes since your todo "
          "list last changed. Update it with todo_write, so that it shows "
          "what is done and what comes next.");
}


// Desktop control (desktop.cpp)
//
// A session's desktop calls go to a helper of its own, started by the
// first and stopped when the turn ends, one call at a time: the request
// goes out as a frame, and the session waits in S_TOOL for the reply,
// at most DESK_ANSWER beyond what the call may take (a wait's seconds).
// The helper's first reply answers hello, which checks its protocol. A
// helper that dies, says nothing in time, or answers with something
// that is no reply fails the call, and the next call starts another.
// A cancel stops the helper at once, except during a drag, which ends
// first so that no mouse button stays held down.
//
// Every call is asked about ([desktop] ask), before its tool.start as a
// path is, with kind "desktop" and the deadline [desktop] ask_timeout.
// Screenshots go to the session's out/ as CALL.png, which the tool.end
// record names with the screenshot's geometry ("shot"), so that a
// session opened again maps points as before, and each request reads
// those it shows ([desktop] images and images_slack) again from there.

enum : i64 {
    DESK_ANSWER = 30*SEC,  // for the helper to answer, beyond a wait
    DESK_REGION = (i64)1 << (sizeof(void *)==8 ? 28 : 27),
};

// Where the helper may be for an executable, first to last: for an
// engine deployed as engine-PROTO-VERSION-OS-ARCH[.exe] (deploy.cpp),
// the helper deployed beside it, desktop-PROTO-VERSION-OS-ARCH[.exe];
// then prospero-desktop[.exe] next to it, as a build leaves it.
static Slice<Str> deskhelpers(Arena *a, Str exe, b32 windows)
{
    Slice<Str> r    = {};
    Str        dir  = parentdir(exe);
    Str        base = basename(exe);
    if (startswith(base, "engine-")) {
        Str name = concat(a, "desktop-", cuthead(base, 7));
        r = push(a, r, join(a, dir, name));
    }
    Str name = windows ? Str("prospero-desktop.exe") : Str("prospero-desktop");
    r = push(a, r, join(a, dir, name));
    return r;
}

// The helper's command line: the first of deskhelpers there, or for
// tests the JSON array of strings in PROSPERO_TEST_DESKTOP. Empty if
// none.
static Slice<Str> deskhelper(Engine *e, Arena *a)
{
    Slice<Str> r    = {};
    Str        test = os_getenv(e->os, a, "PROSPERO_TEST_DESKTOP");
    if (test.len) {
        JsonDoc d = jsonparse(a, test);
        if (d.ok && d.root->type==JSON_ARRAY) {
            for (Json *k = d.root->child; k; k = k->next) {
                r = push(a, r, jsonstr(k));
            }
        }
        return r;
    }
    Slice<Str> paths = deskhelpers(a, e->te.exe, e->te.windows);
    for (iz i = 0; i < paths.len; i++) {
        if (os_stat(e->os, *a, paths[i]).type == STAT_FILE) {
            return push(a, r, paths[i]);
        }
    }
    return r;
}

static Str deskoffer(Engine *e)
{
    return trim(cfgget(e->cfg, "desktop", "", "offer", 0));
}

// The desktop bit of a new session's tools denied: offered to a role
// that lists it, unless offer is none, and to a session without a role
// only when offer is all.
static u32 deskdeny(Engine *e, b32 role)
{
    Str offer = deskoffer(e);
    b32 ok    = role ? offer!="none" : offer=="all";
    return ok ? 0 : (u32)TOOL_OFFMASK;
}

struct DeskLimits {
    i64 edge;
    i64 pixels;
};

// The largest screenshot to send the session's model.
static DeskLimits desklimits(Session *s)
{
    b32        an = s->ep.kind == EP_ANTHROPIC;
    DeskLimits r  = {};
    r.edge   = s->mc.imgedge   ? s->mc.imgedge   : an ? DESK_EDGE   : DESK_EDGE_OAI;
    r.pixels = s->mc.imgpixels ? s->mc.imgpixels : an ? DESK_PIXELS : DESK_PIXELS_OAI;
    return r;
}

// Screenshots kept in requests, and how many more before the oldest
// are dropped (deskdropped).
static i64 deskkeep(Engine *e)
{
    Int v = parseint(trim(cfgget(e->cfg, "desktop", "", "images", 0)));
    return v.ok ? (v.val<0 ? 0 : v.val>20 ? 20 : v.val) : DESK_IMAGES;
}

static i64 deskslack(Engine *e)
{
    Int v = parseint(trim(cfgget(e->cfg, "desktop", "", "images_slack", 0)));
    return v.ok ? (v.val<0 ? 0 : v.val>20 ? 20 : v.val) : DESK_SLACK;
}

// Which screenshots a request leaves out, the oldest first, and where
// its prefix stops matching the request before (FLOOR_*).
struct DeskDrop {
    i64 drop;   // screenshots left out
    i64 read;   // FLOOR_READ, or -1
    i64 write;  // FLOOR_WRITE, or -1
};

static DeskDrop deskdrop(Session *s)
{
    Engine *e     = s->e;
    i64     keep  = deskkeep(e);
    i64     slack = deskslack(e);
    iz      last  = -1;  // the latest response: the request before ended there
    for (iz i = 0; i < s->history.len; i++) {
        last = s->history[i].role==ROLE_ASSISTANT ? i : last;
    }
    i64 total  = 0;
    i64 before = 0;
    for (iz i = 0; i < s->history.len; i++) {
        b32 shot = s->history[i].image.len > 0;
        total  += shot;
        before += shot && i<last;
    }
    DeskDrop r   = {};
    i64      was = deskdropped(before, keep, slack);
    r.drop  = deskdropped(total, keep, slack);
    r.read  = keep>0 && r.drop>was ? was : -1;
    i64 old = keep>0 && before>was ? was : -1;  // the oldest shown, or -1
    i64 now = keep>0 && total>r.drop ? r.drop : -1;
    r.write = now!=old ? now : -1;
    return r;
}

// Tokens of the screenshots the next request carries.
static i64 deskimagetokens(Session *s)
{
    i64 drop = deskdrop(s).drop;
    i64 n    = 0;
    i64 r    = 0;
    for (iz i = 0; i < s->history.len; i++) {
        Msg *m = &s->history[i];
        if (m->image.len && n++>=drop) {
            r += imagetokens(m->imgw, m->imgh);
        }
    }
    return r;
}

// The history for a request: the screenshots shown read in as base64,
// with a short text in place of each left out, and the floors marked.
// The history itself if it has none.
static Slice<Msg> deskimages(Session *s, Arena *a)
{
    Engine *e   = s->e;
    b32     any = 0;
    for (iz i = 0; i<s->history.len && !any; i++) {
        any = s->history[i].image.len > 0;
    }
    if (!any) {
        return s->history;
    }
    DeskDrop   d = deskdrop(s);
    Slice<Msg> h = {};
    h.data = alloc<Msg>(a, s->history.len);
    h.len  = h.cap = s->history.len;
    i64 n  = 0;
    for (iz i = 0; i < s->history.len; i++) {
        Msg m = s->history[i];
        if (m.image.len) {
            m.floor = n==d.read ? FLOOR_READ : n==d.write ? FLOOR_WRITE : FLOOR_NONE;
            if (n++ < d.drop) {
                m.imgalt = concat(a, concat(a, "[screenshot omitted: ", m.callid), "]");
            } else {
                Read rd = os_read(e->os, a, join(a, s->dir, m.image), DESK_REPLYMAX);
                if (rd.err == READ_OK) {
                    m.imgb64 = base64(a, rd.data);
                } else {
                    m.imgalt = concat(a, concat(a, "[screenshot lost: ", m.callid),
                                      ", its file could not be read]");
                }
            }
        }
        h.data[i] = m;
    }
    return h;
}

static Session *deskbyid(Engine *e, u64 id)
{
    for (Session *s = e->sessions; id && s; s = s->next) {
        if (s->desk.id == id) {
            return s;
        }
    }
    return 0;
}

// Forget what the helper sent so far.
static void deskreset(Session *s)
{
    DeskRun *k = &s->desk;
    k->arena = newarena(k->region);
    k->in    = Buf(&k->arena, 1<<16);
}

// Stop the helper at once, its records from then on ignored.
static void deskkill(Session *s)
{
    DeskRun *k = &s->desk;
    if (k->proc) {
        os_kill(s->e->os, k->proc, 1);
        os_procfree(s->e->os, k->proc);
    }
    k->proc  = 0;
    k->id    = 0;
    k->hello = 0;
}

// Let the helper go: the end of its input ends it, unless a call still
// waits for it, which a cancel or a failure deals with first.
static void deskstop(Session *s)
{
    DeskRun *k = &s->desk;
    if (k->busy) {
        deskkill(s);
        k->busy = 0;
    } else if (k->proc) {
        os_procwrite(s->e->os, k->proc, {});
        os_procfree(s->e->os, k->proc);
        k->proc  = 0;
        k->id    = 0;
        k->hello = 0;
    }
}

// Start the helper and greet it. 0 on failure, explained by os_lasterr.
static b32 deskstart(Session *s, Arena scratch)
{
    Engine     *e    = s->e;
    DeskRun    *k    = &s->desk;
    Slice<Str>  argv = deskhelper(e, &scratch);
    argv = push(&scratch, argv, Str("--serve"));
    if (!k->region) {
        k->region = os_reserve(e->os, DESK_REGION);
        if (!k->region) {
            return 0;
        }
    }
    deskreset(s);
    Slice<Str> secrets = cfgsecrets(e->cfg, &scratch);
    Spawn      sp      = {};
    sp.argv      = argv;
    sp.cwd       = s->cwd;
    sp.env       = shellenv(&scratch, e->env, s->te.windows, secrets);
    // Where it keeps what it must remember, as Wayland's portal approval
    sp.env       = push(&scratch, sp.env, concat(&scratch, "PROSPERO_STATE=", e->statedir));
    sp.outpath   = join(&scratch, s->outdir, "desktop.log");
    sp.mbox      = e->in;
    sp.id        = k->id = ++e->nextid;
    sp.breakaway = 1;
    k->proc = os_spawnio(e->os, &sp, scratch);
    if (!k->proc) {
        k->id = 0;
        return 0;
    }
    Slice<Str> hello = {};
    hello = push(&scratch, hello, Str("hello"));
    if (!os_procwrite(e->os, k->proc, deskframe(&scratch, hello))) {
        deskkill(s);
        return 0;
    }
    return 1;
}

static Str deskint(Arena *a, i64 v)
{
    Buf b(a, 24);
    print(&b, v);
    return finish(&b);
}

static void deskxy(Buf *b, f64 x, f64 y)
{
    putbyte(b, '(');
    printfloat(b, x, x==(f64)(i64)x ? 0 : 1);
    print(b, ", ");
    printfloat(b, y, y==(f64)(i64)y ? 0 : 1);
    putbyte(b, ')');
}

// Where input with no window of its own goes: the window named, the
// latest screenshot's window, or the foreground.
static Str deskinput(Session *s, Arena *a, DeskArgs *d)
{
    if (d->window.len) {
        return deskwindow(a, d->window);
    }
    DeskShot *shot = &s->desk.shot;
    return shot->ok && shot->target!="screen" ? shot->target : Str("active");
}

// What a call does, for the user who is asked, and the agent told.
static Str deskwhat(Session *s, Arena *a, DeskArgs *d, b32 done)
{
    DeskShot *shot  = &s->desk.shot;
    Buf       b(a, 256);
    Str       where = shot->ok && shot->target!="screen"
                    ? Str(" in the screenshot of the window")
                    : Str(" in the screenshot of the screen");
    switch (d->action) {
    case DESK_SCREENSHOT:
        print(&b, done ? Str("Took a screenshot of ") : Str("take a screenshot of "));
        if (d->window.len) {
            print(&b, "the window \"");
            print(&b, d->window);
            putbyte(&b, '"');
        } else {
            print(&b, "the screen");
        }
        break;
    case DESK_CLICK:
    case DESK_DOUBLE:
    case DESK_RIGHT:
    case DESK_MOVE: {
        static Str const verbs[][2] = {
            {"click at ", "Clicked at "},
            {"double-click at ", "Double-clicked at "},
            {"right-click at ", "Right-clicked at "},
            {"move the pointer to ", "Moved the pointer to "},
        };
        print(&b, verbs[d->action-DESK_CLICK][done]);
        deskxy(&b, d->x, d->y);
        print(&b, where);
    } break;
    case DESK_DRAG:
        print(&b, done ? Str("Dragged from ") : Str("drag from "));
        deskxy(&b, d->x, d->y);
        print(&b, " to ");
        deskxy(&b, d->x2, d->y2);
        print(&b, where);
        break;
    case DESK_SCROLL:
        print(&b, done ? Str("Scrolled ") : Str("scroll "));
        print(&b, d->amount>0 ? d->amount : -d->amount);
        print(&b, d->amount>0 ? Str(" up at ") : Str(" down at "));
        deskxy(&b, d->x, d->y);
        print(&b, where);
        break;
    case DESK_KEY:
        print(&b, done ? Str("Pressed ") : Str("press "));
        print(&b, trim(d->keys));
        break;
    case DESK_TYPE: {
        Str t = d->text.len>200 ? utf8head(d->text, 200) : d->text;
        print(&b, done ? Str("Typed ") : Str("type "));
        printjsonstr(&b, t);
        print(&b, t.len<d->text.len ? Str("...") : Str(""));
    } break;
    case DESK_WINDOWS:
        print(&b, done ? Str("Listed the windows") : Str("list the windows"));
        break;
    case DESK_FOCUS:
        print(&b, done ? Str("Brought \"") : Str("bring \""));
        print(&b, d->window);
        print(&b, "\" to the front");
        break;
    case DESK_CLOSE:
        print(&b, done ? Str("Asked \"") : Str("ask \""));
        print(&b, d->window);
        print(&b, "\" to close");
        break;
    case DESK_WAIT:
        print(&b, done ? Str("Waited for \"") : Str("wait for \""));
        print(&b, d->window);
        print(&b, d->gone ? Str("\" to be gone") : Str("\" to appear"));
        break;
    case DESK_RUN:
        print(&b, done ? Str("Started ") : Str("run "));
        print(&b, d->program);
        for (iz i = 0; i < d->args.len; i++) {
            putbyte(&b, ' ');
            print(&b, d->args[i]);
        }
        break;
    }
    if ((d->action==DESK_KEY || d->action==DESK_TYPE) && d->window.len) {
        print(&b, " in \"");
        print(&b, d->window);
        putbyte(&b, '"');
    }
    return finish(&b);
}

// Why a call cannot be made, or null if it can.
static Str deskcheck(Session *s, Arena *a, DeskArgs *d)
{
    Engine *e = s->e;
    if (deskoffer(e) == "none") {
        return "Desktop control is off: the configuration's [desktop] offer "
               "is none, so nothing was done.";
    } else if (!deskhelper(e, a).len) {
        return "Desktop control is not available here: there is no desktop "
               "helper (prospero-desktop) next to the harness, which so far "
               "exists for Windows, macOS and Linux (X11 and Wayland) only. "
               "Nothing was done.";
    } else if (d->action==DESK_SCREENSHOT && s->mc.novision) {
        return "This session's model takes no images (its [model] vision is "
               "false), so it cannot see screenshots.";
    }
    if (deskneedspoint(d->action)) {
        DeskShot *shot = &s->desk.shot;
        if (!shot->ok) {
            return "Take a screenshot first: points are in the latest "
                   "screenshot, and this session has none yet. Nothing was "
                   "done.";
        }
        DeskPoint p = desktarget(a, shot, s->te.coords, d->x, d->y);
        if (!p.why.data && d->action==DESK_DRAG) {
            p = desktarget(a, shot, s->te.coords, d->x2, d->y2);
        }
        if (p.why.data) {
            return concat(a, concat(a, "Not done: ", p.why), ". Use a point "
                          "in the latest screenshot.");
        }
    }
    return {};
}

// Send the current call to the helper, starting it if need be. Returns
// 1 if the call finished (it could not be sent), 0 if it waits for the
// reply.
static b32 deskrun(Session *s, Arena scratch, DeskArgs *d)
{
    Engine    *e    = s->e;
    DeskRun   *k    = &s->desk;
    DeskShot  *shot = &k->shot;
    i32        cs   = s->te.coords;
    Str        win  = d->window.len ? deskwindow(&scratch, d->window) : Str{};
    Slice<Str> argv = {};
    i64        wait = 0;
    Str        why  = deskcheck(s, &scratch, d);  // the configuration may change
    if (why.data) {
        toolend(s, scratch, "error", why, 0, 0, {}, 0);
        return 1;
    }

    static Str const clicks[] = {"click", "dclick", "rclick", "hover"};
    DeskPoint p = {};
    switch (d->action) {
    case DESK_SCREENSHOT: {
        DeskLimits l = desklimits(s);
        argv = push(&scratch, argv, Str("shot"));
        argv = push(&scratch, argv, Str("-s"));
        argv = push(&scratch, argv, deskint(&scratch, l.edge));
        argv = push(&scratch, argv, deskint(&scratch, l.pixels));
        argv = push(&scratch, argv, win.len ? win : Str("screen"));
    } break;
    case DESK_CLICK:
    case DESK_DOUBLE:
    case DESK_RIGHT:
    case DESK_MOVE:
        p    = desktarget(&scratch, shot, cs, d->x, d->y);
        argv = push(&scratch, argv, clicks[d->action-DESK_CLICK]);
        argv = push(&scratch, argv, shot->target);
        argv = push(&scratch, argv, deskint(&scratch, p.x));
        argv = push(&scratch, argv, deskint(&scratch, p.y));
        break;
    case DESK_DRAG: {
        DeskPoint q = desktarget(&scratch, shot, cs, d->x2, d->y2);
        p    = desktarget(&scratch, shot, cs, d->x, d->y);
        argv = push(&scratch, argv, Str("drag"));
        argv = push(&scratch, argv, shot->target);
        argv = push(&scratch, argv, deskint(&scratch, p.x));
        argv = push(&scratch, argv, deskint(&scratch, p.y));
        argv = push(&scratch, argv, deskint(&scratch, q.x));
        argv = push(&scratch, argv, deskint(&scratch, q.y));
    } break;
    case DESK_SCROLL:
        p    = desktarget(&scratch, shot, cs, d->x, d->y);
        argv = push(&scratch, argv, Str("wheel"));
        argv = push(&scratch, argv, shot->target);
        argv = push(&scratch, argv, deskint(&scratch, p.x));
        argv = push(&scratch, argv, deskint(&scratch, p.y));
        argv = push(&scratch, argv, deskint(&scratch, d->amount));
        break;
    case DESK_KEY:
        argv = push(&scratch, argv, Str("key"));
        argv = push(&scratch, argv, deskinput(s, &scratch, d));
        for (Str rest = trim(d->keys); rest.len;) {
            iz i = 0;
            for (; i<rest.len && !whitespace(rest[i]); i++) {}
            argv = push(&scratch, argv, takehead(rest, i));
            rest = trimleft(cuthead(rest, i));
        }
        break;
    case DESK_TYPE:
        argv = push(&scratch, argv, Str("type"));
        argv = push(&scratch, argv, deskinput(s, &scratch, d));
        argv = push(&scratch, argv, d->text);
        break;
    case DESK_WINDOWS:
        argv = push(&scratch, argv, Str("list"));
        if (win.len) {
            argv = push(&scratch, argv, win);
        }
        break;
    case DESK_FOCUS:
    case DESK_CLOSE:
        argv = push(&scratch, argv, d->action==DESK_FOCUS ? Str("raise") : Str("close"));
        argv = push(&scratch, argv, win);
        break;
    case DESK_WAIT:
        wait = d->secs>0 ? d->secs : 10;
        argv = push(&scratch, argv, Str("wait"));
        if (d->gone) {
            argv = push(&scratch, argv, Str("-gone"));
        }
        argv = push(&scratch, argv, Str("-t"));
        argv = push(&scratch, argv, deskint(&scratch, wait));
        argv = push(&scratch, argv, win);
        break;
    case DESK_RUN:
        argv = push(&scratch, argv, Str("run"));
        argv = push(&scratch, argv, d->program);
        for (iz i = 0; i < d->args.len; i++) {
            argv = push(&scratch, argv, d->args[i]);
        }
        break;
    }

    if (!k->proc && !deskstart(s, scratch)) {
        Str err  = os_lasterr(e->os, &scratch);
        Str text = concat(&scratch, "The desktop helper could not be started: ", err);
        toolend(s, scratch, "error", text, 0, 0, {}, 0);
        return 1;
    } else if (!os_procwrite(e->os, k->proc, deskframe(&scratch, argv))) {
        deskkill(s);
        toolend(s, scratch, "error", "The desktop helper stopped taking "
                "commands, so nothing was done. Try again.", 0, 0, {}, 0);
        return 1;
    }
    k->busy   = 1;
    k->action = d->action;
    arm(s, T_DESK, DESK_ANSWER + wait*SEC);
    setstate(s, S_TOOL);
    return 0;
}

// The helper's list of windows as the agent reads them.
static Str deskwindows(Arena *a, Str list)
{
    Buf b(a, list.len + 256);
    for (Str rest = list; rest.len;) {
        Cut line = cut(rest, '\n');
        rest = line.tail;
        Str f[6] = {};
        Str r    = line.head;
        for (i32 i = 0; i < 6; i++) {
            Cut c = cut(r, '\t');
            f[i] = c.head;
            r    = c.tail;
            if (!c.ok) break;
        }
        if (!f[0].len) {
            continue;
        }
        print(&b, "id=");
        print(&b, f[0]);
        print(&b, " pid=");
        print(&b, f[1]);
        putbyte(&b, ' ');
        print(&b, f[3]);
        putbyte(&b, ' ');
        print(&b, f[4]);
        if (f[5].len) {
            print(&b, " \"");
            print(&b, f[5]);
            putbyte(&b, '"');
        }
        putbyte(&b, '\n');
    }
    if (!b.len) {
        return "No window matches.";
    }
    b.len--;
    return finish(&b);
}

// The reply to the current call: its result, then the next call.
static void deskanswer(Session *s, Arena scratch, DeskReply *r)
{
    Engine  *e = s->e;
    DeskRun *k = &s->desk;
    Block   *c = &s->calls[s->callidx];
    Args     a = parseargs(&scratch, TOOL_DESKTOP, c->args, s->te.edit);
    DeskArgs *d = &a.desk;
    disarm(s, T_DESK);
    k->busy = 0;
    setstate(s, S_IDLE);
    if (desklooks(d->action)) {
        s->seen = deskseen(s->seen, r);
    }

    Str what = deskwhat(s, &scratch, d, 1);
    Str text = {};
    if (!r->ok) {
        deskkill(s);
        text = "The desktop helper's reply made no sense, so it was stopped.";
    } else if (r->status) {
        Buf b(&scratch, 256 + r->text.len);
        print(&b, r->status==3 ? Str("Timed out: ") : Str("Failed: "));
        print(&b, trim(r->text));
        print(&b, r->status==2 ? Str(". The windows action lists the windows "
                                     "there are, with their ids.")
                               : Str("."));
        text = finish(&b);
    }
    if (text.data) {
        toolend(s, scratch, "error", text, 0, 0, {}, 0);
        nexttool(s, scratch);
        return;
    }

    Buf b(&scratch, 512 + r->text.len);
    print(&b, what);
    switch (d->action) {
    case DESK_SCREENSHOT: {
        DeskShot sh = deskshotparse(&scratch, r->text);
        DeskSize ps = pngsize(r->data);
        if (!sh.ok || ps.w!=sh.iw || ps.h!=sh.ih) {
            toolend(s, scratch, "error", "The desktop helper sent a "
                    "screenshot that is not one.", 0, 0, {}, 0);
            nexttool(s, scratch);
            return;
        }
        Str rel  = {};
        Str path = spillfile(s, scratch, c->id, &rel, ".png");
        if (!os_write(e->os, scratch, path, r->data)) {
            Str err = os_lasterr(e->os, &scratch);
            text = concat(&scratch, "The screenshot could not be saved: ", err);
            toolend(s, scratch, "error", text, 0, 0, {}, 0);
            nexttool(s, scratch);
            return;
        }
        sh.target = clone(&s->perm, sh.target);
        k->shot   = sh;
        s->callimage = rel;
        s->callimgw  = sh.iw;
        s->callimgh  = sh.ih;
        s->callshot  = clone(&s->perm, deskshotstr(&scratch, &sh));
        print(&b, ": ");
        print(&b, (i64)sh.w);
        putbyte(&b, 'x');
        print(&b, (i64)sh.h);
        print(&b, " pixels, ");
        if (sh.iw==sh.w && sh.ih==sh.h) {
            print(&b, "shown to you as they are.");
        } else {
            print(&b, "shown to you scaled down to ");
            print(&b, (i64)sh.iw);
            putbyte(&b, 'x');
            print(&b, (i64)sh.ih);
            putbyte(&b, '.');
        }
        if (s->te.coords == COORDS_NORM1000) {
            print(&b, " Give points in thousandths of its width and height, "
                      "0 to 1000.");
        } else {
            print(&b, " Give points in pixels of this image: x from 0 to ");
            print(&b, (i64)(sh.iw - 1));
            print(&b, ", y from 0 to ");
            print(&b, (i64)(sh.ih - 1));
            putbyte(&b, '.');
        }
    } break;
    case DESK_WINDOWS:
        print(&b, ":\n");
        print(&b, deskwindows(&scratch, r->text));
        break;
    case DESK_WAIT:
        print(&b, d->gone ? Str(": it is gone.") : Str(": it is there.\n"));
        if (!d->gone) {
            print(&b, deskwindows(&scratch, r->text));
        }
        break;
    case DESK_RUN: {
        // The pid, and a line more if it could not be detached
        Cut line = cut(trim(r->text), '\n');
        Str pid  = trim(line.head);
        Str note = trim(line.tail);
        print(&b, ", process id ");
        print(&b, pid);
        print(&b, ". Wait for its window (wait, with window pid=");
        print(&b, pid);
        print(&b, ") before you use it.");
        if (note.len) {
            print(&b, " Note: ");
            print(&b, note);
            putbyte(&b, '.');
        }
    } break;
    default:
        putbyte(&b, '.');
    }
    toolend(s, scratch, "ok", finish(&b), 0, 0, {}, 0);
    nexttool(s, scratch);
}

// The helper failed the current call, or went away: end it with why.
static void deskbroke(Session *s, Arena scratch, Str why)
{
    DeskRun *k    = &s->desk;
    b32      busy = k->busy;
    Str      log  = {};
    if (k->proc) {
        Str  err = join(&scratch, s->outdir, "desktop.log");
        Read rd  = os_read(s->e->os, &scratch, err, 4096);
        log = rd.err==READ_OK ? trim(rd.data) : Str{};
    }
    deskkill(s);
    k->busy = 0;
    if (!busy) {
        return;
    }
    disarm(s, T_DESK);
    setstate(s, S_IDLE);
    Buf b(&scratch, 256 + log.len);
    print(&b, why);
    if (log.len) {
        print(&b, " It said: ");
        print(&b, log);
    }
    toolend(s, scratch, "error", finish(&b), 0, 0, {}, 0);
    nexttool(s, scratch);
}

// A record of the session's helper: its output, or its end.
static void deskrecord(Session *s, Arena scratch, Record r)
{
    DeskRun *k = &s->desk;
    if (r.type == PROC_TAIL || r.type == PROC_LIMIT) {
        return;  // not for a helper
    } else if (r.type != PROC_DATA) {
        deskbroke(s, scratch, "The desktop helper stopped, so the action "
                  "may not have been done.");
        return;
    }

    print(&k->in, r.data);
    // Whole frames out, the rest kept, before any is acted upon, which
    // may restart the helper
    Slice<Str> frames = {};
    for (;;) {
        Str buf = {k->in.data, k->in.len};
        iz  n   = deskframelen(buf, DESK_REPLYMAX);
        if (n < 0) {
            deskbroke(s, scratch, "The desktop helper sent a reply too "
                      "large, so it was stopped.");
            return;
        } else if (!n) {
            break;
        }
        frames = push(&scratch, frames, clone(&scratch, takehead(buf, n)));
        for (iz i = n; i < buf.len; i++) {
            k->in.data[i-n] = k->in.data[i];
        }
        k->in.len -= n;
    }
    if (!k->in.len && k->in.cap>(1<<20)) {
        deskreset(s);  // let a large reply's room go
    }

    u64 id = k->id;
    for (iz i = 0; i<frames.len && k->id==id; i++) {
        DeskReply rp = deskreply(frames[i]);
        if (!k->hello) {
            Buf want(&scratch, 32);
            print(&want, "prospero-desktop ");
            print(&want, (i64)DESK_PROTO);
            putbyte(&want, ' ');
            k->hello = 1;
            if (!rp.ok || rp.status || !startswith(rp.text, finish(&want))) {
                Str text = rp.ok ? trim(rp.text) : Str("(nothing)");
                deskbroke(s, scratch, concat(&scratch, "The desktop helper "
                          "is not one this harness can use, so it was "
                          "stopped. Its hello: ", text));
                return;
            }
        } else if (k->busy) {
            deskanswer(s, scratch, &rp);
        }
    }
}

// A desktop call, asked about first if need be. Returns 1 if the call
// finished, 0 if it goes on (asking, or waiting for the helper).
static b32 deskcall(Session *s, Arena scratch, DeskArgs *d)
{
    Engine *e   = s->e;
    Block  *c   = &s->calls[s->callidx];
    Str     why = deskcheck(s, &scratch, d);
    if (why.data) {
        toolstart(s, scratch);
        toolend(s, scratch, "error", why, 0, 0, {}, 0);
        return 1;
    } else if (!cfgbool(cfgget(e->cfg, "desktop", "", "ask", 0))) {
        toolstart(s, scratch);
        return deskrun(s, scratch, d);
    }

    Ask *k = &s->ask;
    *k = {};
    k->req    = clone(&s->perm, concat(&scratch, c->id, "/ask"));
    k->tool   = TOOL_DESKTOP;
    k->opened = os_ticks(e->os);
    b32 steer = s->steers.len > 0;
    b32 wait  = attending(s) && !steer;
    i64 delay = cfgms(e->cfg, "desktop", "", "ask_timeout", 0)*MS;
    if (wait) {
        arm(s, T_ASK, delay);
    }
    {
        Str   what = concat(&scratch, "desktop: ", deskwhat(s, &scratch, d, 0));
        Buf   b(&scratch, 512 + what.len);
        EvOut w = sbegin(s, &b, EV_ASK);
        putstrkey(&w.json, "req", k->req);
        putstrkey(&w.json, "call", c->id);
        putstrkey(&w.json, "kind", "desktop");
        putstrkey(&w.json, "tool", "desktop");
        putstrkey(&w.json, "detail", what);
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

// Why a desktop call was not made, for the agent.
static Str deskdenial(Arena *a, i32 by, Str note, i64 wait)
{
    Buf b(a, 512 + note.len);
    switch (by) {
    case DENY_POLICY:
        print(&b, "Not done: desktop actions need the user's approval, and "
                  "nobody is there to give it in this run. Do what you can "
                  "without the desktop, and say what you would have done.");
        break;
    case DENY_USER:
        print(&b, "Not done: the user denied this desktop action.");
        if (note.len) {
            print(&b, " The user's note: ");
            print(&b, note);
        } else {
            print(&b, " Do not try it again unless the user asks.");
        }
        break;
    case DENY_TIMEOUT:
        print(&b, "Not done: desktop actions need the user's approval, and "
                  "nobody answered within ");
        print(&b, wait);
        print(&b, " seconds.");
        break;
    case DENY_STEER:
        print(&b, "Not done: the user sent a new message instead of "
                  "answering. Read that message (it follows these results) "
                  "and do what it says.");
        break;
    case DENY_CANCEL:
        print(&b, "Not run: the user cancelled the turn.");
        break;
    }
    return finish(&b);
}


// Run one call. Returns 1 if it finished, 0 if it goes on (a command or
// a search, or an ask waiting for the user).
static b32 runtool(Session *s, Arena scratch)
{
    Engine *e    = s->e;
    Block  *c    = &s->calls[s->callidx];
    i32     tool = toolfind(c->name);
    Args    args = {};
    if (!tooloffered(&s->te, tool)) {
        tool = TOOL_NONE;  // one the role leaves out is unknown
    } else {
        args = parseargs(&scratch, tool, c->args, s->te.edit);
    }

    // The guard comes first: a path it questions is asked about before
    // the call starts. A search without a path searches the working
    // directory. A patch may change files outside the roots only when it
    // names one such file, which is asked about.
    if (!s->blocked && tool==TOOL_EDIT && s->te.edit==EDIT_PATCH && !args.issues.len) {
        PatchGuard pg = patchguard(s, &scratch, &args.patch, {});
        if (pg.many) {
            toolstart(s, scratch);
            toolend(s, scratch, "denied", "Not run: the patch changes more than "
                    "one file outside the working directory. Change each of "
                    "them with a patch of its own, so that each can be "
                    "allowed.", 0, 0, {}, 0);
            return 1;
        } else if (pg.out.path.data) {
            return askcall(s, scratch, tool, &pg.out);
        }
        toolstart(s, scratch);
        return runfile(s, scratch, tool, s->guard.cwd, &args);
    }
    if (!s->blocked && tool!=TOOL_NONE && toolhaspath(tool) && !args.issues.len) {
        Str       want = args.path.data ? args.path : Str(".");
        GuardPath g    = guardpath(e->os, &scratch, &s->guard, want, toolreads(tool));
        if (!g.inside) {
            return askcall(s, scratch, tool, &g);
        }
        toolstart(s, scratch);
        return runfile(s, scratch, tool, g.path, &args);
    }

    if (!s->blocked && tool==TOOL_DESKTOP && !args.issues.len) {
        return deskcall(s, scratch, &args.desk);  // asks before it starts
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
        s->callfail = "loop";
        toolend(s, scratch, "error", text, 0, 0, {}, 0);
        return 1;
    }

    if (tool == TOOL_NONE) {
        Str text = unknowntool(&scratch, c->name, s->te.deny);
        s->callfail = "unknown";
        toolend(s, scratch, "error", text, 0, 0, {}, 0);
        return 1;
    }
    if (args.issues.len) {
        s->callfail = "args";
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

    switch (tool) {
    case TOOL_TODO:    tooltodo(s, scratch, &args);    return 1;
    case TOOL_JOBS:    tooljobs(s, scratch);           return 1;
    case TOOL_JOBOUT:  tooljobout(s, scratch, &args);  return 1;
    case TOOL_JOBKILL: tooljobkill(s, scratch, &args); return 1;
    case TOOL_TASK:    assert(0);  // starttasks has these
    case TOOL_DESKTOP: assert(0);  // deskcall has these
    }
    if (args.background) {
        startjob(s, scratch, &args);
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
    todoremind(s, scratch);
    prepare(s, scratch);
}

static void nexttool(Session *s, Arena scratch)
{
    while (s->callidx < s->calls.len) {
        Arena a = scratch;
        if (s->state==S_TOOL || s->state==S_TASK) {
            return;  // a call already runs (one an answer allowed)
        }
        if (s->cancelled) {
            toolstart(s, a);
            toolend(s, a, "cancelled", "Not run: the user cancelled the turn.",
                    0, 0, {}, 0);
            continue;
        }
        if (s->calls[s->callidx].name=="task" && !s->blocked &&
                tooloffered(&s->te, TOOL_TASK)) {
            starttasks(s, a);  // they end as their subagents report
            return;
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
    s->thinkfrom = s->log.seq;
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
    i64 *k = upsert(&e->kinds, kindkey(&scratch, cp->ep.url, cp->mc.name), (Arena *)0);
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
    b32 an    = cp->ep.kind == EP_ANTHROPIC;
    Str body  = an ? ansummary(&s->attempt, scratch, &cp->mc, prev, convo, words)
                   : encodesummary(&s->attempt, scratch, &cp->mc, prev, convo, words);
    Str url   = apiurl(&scratch, cp->ep.url, an ? Str("messages")
                                                : Str("chat/completions"));
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
// would make no headway, so the turn ends instead. A summary declined
// (refusal) or cut off (length) is never used, since the history it
// stands for would be gone for good, and asking again would end alike.
static void compacted(Session *s, Arena scratch, Partial *p)
{
    Engine *e    = s->e;
    Compact cp   = s->cp;
    Str     text = trim(summarytext(trim(finish(&p->text))));
    if (p->finish=="refusal" || p->finish=="length") {
        b32 refused = p->finish == "refusal";
        Buf b(&scratch, 512 + p->refusal.len);
        print(&b, "The summarizer, ");
        print(&b, cp.model);
        if (refused) {
            print(&b, ", declined to summarize the conversation (stop "
                      "reason refusal");
            if (p->refusal.len) {
                print(&b, ", ");
                print(&b, p->refusal);
            }
            print(&b, ")");
        } else {
            print(&b, ", stopped at its output limit before the summary "
                      "was done");
        }
        print(&b, ", so the conversation was not compacted and the turn "
                  "ends here. Set [model] compactor to another model");
        print(&b, refused ? Str(".") : Str(", or raise its max_tokens."));
        note(s, scratch, "error", finish(&b));
        Str why = refused ? Str("the summarizer declined (refusal)")
                          : Str("the summary was cut off at the length limit");
        failcompact(s, scratch, CLASS_DETERMINISTIC, 0, why, -1);
        return;
    } else if (!text.len) {
        failcompact(s, scratch, CLASS_TRANSIENT, 0, "the summary was empty", -1);
        return;
    }
    iz most = (iz)bytesof(s->rate, COMPACT_SUMMAX);
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
    s->thinkfrom = s->log.seq;
    s->purpose   = REQ_CHAT;
    s->cp        = {};
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
    if (p->finish == "refusal") {
        p->calls = {};  // a declined response's calls are not run
    }
    Slice<Str> used = usedids(s, &scratch);
    Finished   f    = finishpartial(&s->perm, p, 1, s->turn, s->steps, used);
    Str        fr   = p->finish.data ? p->finish : Str("stop");
    if (fr == "refusal") {
        // Shown and logged, but never sent back: a declined response's
        // partial output is not a finished turn of the conversation
        logassistant(s, scratch, f.blocks, fr, p->hasusage ? &p->usage : 0);
    } else {
        addassistant(s, scratch, f.blocks, fr, p->hasusage ? &p->usage : 0);
    }
    setstate(s, S_IDLE);

    // The router has just loaded the model, if it was not loaded: its
    // listing now reports the context, so look again before the next step
    if (s->info.llamacpp && !s->info.learned) {
        s->discovered = 0;
    }

    if (fr == "refusal") {
        // Anthropic's model, or its safeguards, declined: another try
        // would decline alike
        Buf b(&scratch, 256 + p->refusal.len);
        print(&b, "The model declined to continue (stop reason refusal");
        if (p->refusal.len) {
            print(&b, ", ");
            print(&b, p->refusal);
        }
        print(&b, "), so the turn ends here. Rephrase the request, or use "
                  "another model.");
        note(s, scratch, "error", finish(&b));
        endturn(s, scratch, "error", "the model declined to continue (refusal)");
        return;
    } else if (fr=="pause_turn" && !f.ncalls) {
        // The server paused a long turn: sent back as it is, with no
        // message after it, the response goes on where it stopped
        prepare(s, scratch);
        return;
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
        } else if (!todonudge(s, scratch)) {
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
    if (desklooking(s, scratch, calls)) {
        // Looking again repeats only if the last look saw no change
        set ^= (s->seen + 1) * 0x9e3779b97f4a7c15u;
    }
    s->setrepeat = set==s->lastset ? s->setrepeat+1 : 1;
    s->lastset   = set;
    s->blocked   = s->setrepeat >= 5;
    if (!s->blocked) {
        s->seen = 0;  // these calls run and see anew
    }
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
    } else if (s->intask && !taskbudget(s, scratch)) {
        return;
    }
    deliver(s, scratch);
    s->wake = 0;

    s->steps++;
    s->retries = 0;
    i64 hard = s->stepmax ? s->stepmax : STEP_HARD;
    if (s->steps==STEP_SOFT && hard>STEP_SOFT) {
        Buf b(&scratch, 128);
        print(&b, "this turn has made 200 model requests; it ends at ");
        print(&b, hard);
        note(s, scratch, "warn", finish(&b));
    } else if (s->steps > hard) {
        Buf b(&scratch, 128);
        print(&b, "the turn reached its limit of ");
        print(&b, hard);
        print(&b, hard==1 ? Str(" model request") : Str(" model requests"));
        endturn(s, scratch, "budget", finish(&b));
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

// Begin a turn, after its user input if any: via names what began it
// without input ("job": notices of background jobs), else is null.
static void beginturn(Session *s, Arena scratch, Str via)
{
    s->cancelled = 0;
    s->turn++;
    {
        Arena a = scratch;
        Buf   b(&a, 128);
        EvOut w = sbegin(s, &b, EV_TURN);
        putintkey(&w.json, "n", s->turn);
        if (via.len) {
            putstrkey(&w.json, "via", via);
        }
        ssend(s, &w);
    }
    s->steps      = 0;
    s->attempts   = 0;
    s->setrepeat  = 0;
    s->lastset    = 0;
    s->seen       = 0;
    s->discovered = s->info.learned || (s->info.found && !s->info.llamacpp);
    resetnudges(s, 1);
    instrcheck(s, scratch);
    prepare(s, scratch);
}

static void startturn(Session *s, Arena scratch, Str text, Str via)
{
    s->cancelled = 0;
    adduser(s, scratch, text, via);
    if (!s->title.len) {
        Str title = maketitle(&scratch, text);
        settitle(s, scratch, title);
    }
    beginturn(s, scratch, {});
}

// The session needs its model at its endpoint no longer: let it stop
// lingering, unless another session's turn runs on it between requests
// (a tool), since a lane's linger is shared by every session on it.
static void cool(Session *s)
{
    if (!s->point) {
        return;
    }
    for (Session *t = s->e->sessions; t; t = t->next) {
        if (t!=s && t->point==s->point && t->slotmodel==s->slotmodel &&
                t->slot==SLOT_NONE && t->state!=S_TASK && busy(t)) {
            return;
        }
    }
    admcool(s->point, s->slotmodel);
    pump(s->e, s->point);
}

static void endturn(Session *s, Arena scratch, Str why, Str err)
{
    release(s, 0);
    stopreq(s);
    deskstop(s);
    cool(s);  // no next step to keep it for
    for (i32 t = 0; t < T_COUNT; t++) {
        if (t != T_JOBS) {
            disarm(s, t);  // jobs outlive turns
        }
    }
    s->part     = 0;
    s->purpose  = REQ_CHAT;
    s->cp       = {};
    s->slowing  = 0;
    s->slowfrom = 0;
    s->slown    = 0;
    // A job that ended after the turn's last request starts the next
    b32 resume = !s->steers.len && !s->queued.len && !s->closed && s->wake &&
                 s->notices.len && canresume(s);
    {
        Arena a = scratch;
        Buf   b(&a, 256 + err.len);
        EvOut w = sbegin(s, &b, EV_TURNEND);
        putintkey(&w.json, "turn", s->turn);
        putstrkey(&w.json, "why", why);
        if (err.len) {
            putstrkey(&w.json, "err", err);
        }
        if (resume) {
            putintkey(&w.json, "resume", 1);
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
    } else if (s->closed) {
        stopjobs(s, scratch, 0);  // started after the session closed
    } else if (resume) {
        autoresume(s, scratch);
    }
    if (!busy(s)) {
        arm(s, T_PARK, parkafter(s->e));
    }
    taskcheck(s, scratch, why, err);
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
        if (s->desk.busy && s->desk.action==DESK_DRAG) {
            return;  // its button released first: the rest end after it
        } else if (s->desk.busy) {
            deskkill(s);
            s->desk.busy = 0;
            disarm(s, T_DESK);
            setstate(s, S_IDLE);
            toolend(s, scratch, "cancelled", "Stopped: the user cancelled "
                    "the turn, perhaps before the action was done.", 0, 0, {}, 0);
            nexttool(s, scratch);
            return;
        }
        finishshell(s, scratch, 1);
        nexttool(s, scratch);  // the rest are not run, then the turn ends
        return;
    case S_ASK:
        resolve(s, scratch, 0, DENY_CANCEL, {});
        nexttool(s, scratch);
        return;
    case S_TASK:
        taskcancel(s, scratch);
        return;  // the calls end as the subagents report, then the turn
    case S_HEAD:
    case S_STREAM:
    case S_ERRBODY:
        if (s->purpose==REQ_CHAT && s->part && committed(s->part)) {
            s->part->calls = {};
            Slice<Str> used = usedids(s, &scratch);
            Finished   f    = finishpartial(&s->perm, s->part, 1, s->turn,
                                            s->steps, used);
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
    Str kind      = trim(cfgget(cfg, "endpoint", name, "kind", 0));
    ep->kind      = kind=="anthropic" ? EP_ANTHROPIC : EP_OPENAI;
    return 1;
}

static void modelconf(Engine *e, Arena *perm, Str model, ModelConf *mc)
{
    static Str const sampling[] = {
        "temperature", "top_p", "top_k", "min_p", "presence_penalty",
        "frequency_penalty", "repeat_penalty", "seed", "max_tokens",
    };
    Config *cfg = e->cfg;
    mc->name   = clone(perm, cfgmodelid(cfg, model));
    mc->kwargs = cfgtemplate(cfg, perm, model);
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
    mc->edit     = editformat(cfgget(cfg, "model", model, "edit_format", 0));

    // For Anthropic's Messages API
    Int mt  = parseint(trim(cfgget(cfg, "model", model, "max_tokens", 0)));
    Str th  = trim(cfgget(cfg, "model", model, "thinking", 0));
    Str td  = trim(cfgget(cfg, "model", model, "thinking_display", 0));
    Str ef  = trim(cfgget(cfg, "model", model, "effort", 0));
    mc->maxtokens = mt.ok && mt.val>0 ? mt.val : 0;
    mc->think     = th=="budget"        ? THINK_BUDGET  :
                    th=="off"           ? THINK_OFF     :
                    th=="between_tools" ? THINK_BETWEEN :
                    th=="default"       ? THINK_DEFAULT : THINK_ADAPTIVE;
    mc->hidethink = td == "omitted";
    mc->effort    = ef.len ? clone(perm, ef) : Str{};

    // For the desktop tool's screenshots
    Str co = trim(cfgget(cfg, "model", model, "coordinates", 0));
    Int ie = parseint(trim(cfgget(cfg, "model", model, "image_edge", 0)));
    Int ip = parseint(trim(cfgget(cfg, "model", model, "image_pixels", 0)));
    mc->coords    = co=="norm1000" ? COORDS_NORM1000 : COORDS_PIXELS;
    mc->novision  = !cfgbool(cfgget(cfg, "model", model, "vision", 0));
    mc->imgedge   = ie.ok && ie.val>=16 && ie.val<=16384 ? ie.val : 0;
    mc->imgpixels = ip.ok && ip.val>=256 && ip.val<=(i64)1<<28 ? ip.val : 0;
}

static Session *newsession(Engine *e, Arena scratch, Str sid)
{
    Os     *os  = e->os;
    b32     big = sizeof(void *) == 8;
    Region *r1  = os_reserve(os, (iz)1<<(big ? 32 : 26));
    Region *r2  = os_reserve(os, (iz)1<<(big ? 30 : 26));
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
    deskkill(s);
    if (s->desk.region) {
        os_release(e->os, s->desk.region);
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

// The job limits the session's tools describe, from the configuration.
static void jobconf(Session *s)
{
    Config *cfg = s->e->cfg;
    i64     dft = cfgms(cfg, "tools", "", "job_timeout", 0) / 1000;
    i64     max = cfgms(cfg, "tools", "", "job_timeout_max", 0) / 1000;
    max = max<1 ? 1 : max;
    s->te.jobmax     = max;
    s->te.jobtimeout = dft<1 ? 1 : dft>max ? max : dft;
}

// Discover the session's instruction files, unless its log told which
// already (loadsession). The record of them waits for the session's own
// record to be in place (instrlogstart).
static void instrstart(Session *s, Arena scratch)
{
    Engine *e = s->e;
    if (s->instr.data) {
        return;
    }
    Str          cwd    = normpath(&scratch, s->cwd, ".", s->te.windows);
    Slice<Str>   names  = instrlist(s, &scratch);
    Str          global = s->projinstr ? Str{} : e->global;
    Instructions in     = instructions(e->os, &scratch, cwd, global, names,
                                       e->home, s->te.windows);
    s->instr = in.text.data ? clone(&s->perm, in.text) : Str{(u8 *)"", 0};
    instrstamp(s, in.files);

    Instructions *p = &s->instrpend;
    *p = {};
    for (iz i = 0; i < in.files.len; i++) {
        InstrFile f = in.files[i];
        f.text     = {};
        f.path     = clone(&s->perm, f.path);
        f.importer = f.importer.data ? clone(&s->perm, f.importer) : Str{};
        p->files   = push(&s->perm, p->files, f);
    }
    for (iz i = 0; i < in.skipped.len; i++) {
        p->skipped = push(&s->perm, p->skipped, clone(&s->perm, in.skipped[i]));
    }
    s->instrlog = 1;
}

static void instrlogstart(Session *s, Arena scratch)
{
    if (s->instrlog && !s->readonly) {
        s->instrlog = 0;
        loginstr(s, scratch, s->instrpend.files, s->instrpend.skipped,
                 "start", s->instr);
    }
}

static void taskroles(Engine *, Arena *, Arena, ToolEnv *);

// What every request of the session begins with, the system prompt and
// the tools, built from the session and the configuration as they are
// now, and the job limits the tools describe.
static void buildprefix(Session *s, Arena scratch)
{
    Engine *e = s->e;
    jobconf(s);
    s->te.coords = s->mc.coords;
    if (tooloffered(&s->te, TOOL_TASK)) {
        taskroles(e, &s->perm, scratch, &s->te);
    }
    s->system = systemprompt(&s->perm, s->cwd, &s->te);
    s->tools  = tooljson(&s->perm, &s->te);
    if (s->rolepr.len) {
        s->system = concat(&s->perm, s->system, "\n\n# Your role: ");
        s->system = concat(&s->perm, s->system, s->role);
        s->system = concat(&s->perm, s->system, "\n\n");
        s->system = concat(&s->perm, s->system, s->rolepr);
    }
    if (s->depth) {
        s->system = concat(&s->perm, s->system,
            "\n\n# Subagent\n\nYou are a subagent: another agent gave you "
            "the task in the first message, and waits for your report, "
            "which is your final response. It sees nothing else of your "
            "work, and it cannot answer questions, so do not ask any. Do "
            "the task, then report what you did and found: the facts, file "
            "paths and details it needs, and anything you could not do. "
            "When you are told to wrap up, finish at once with your report.");
    }
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
    if (s->instr.len) {
        s->system = concat(&s->perm, s->system,
                           "\n\n# Instructions\n\nThe user and the project "
                           "gave these instructions. Follow them; where they "
                           "disagree, a file in a deeper directory wins over "
                           "those above it.");
        s->system = concat(&s->perm, s->system, s->instr);
    }
}

// The prefix's durable record, so that the session opened again, or
// revived from parking, sends it byte for byte whatever the configuration
// says then: only a compaction may break the prompt cache.
static void logprefix(Session *s, Arena scratch)
{
    if (s->readonly) {
        return;
    }
    Buf   b(&scratch, 1024 + 2*(s->system.len + s->tools.len));
    EvOut w = sbegin(s, &b, EV_PREFIX);
    putstrkey(&w.json, "system", s->system);
    putstrkey(&w.json, "tools", s->tools);
    putintkey(&w.json, "job_timeout", s->te.jobtimeout);
    putintkey(&w.json, "job_timeout_max", s->te.jobmax);
    if (tooloffered(&s->te, TOOL_DESKTOP)) {
        putstrkey(&w.json, "coordinates", s->te.coords==COORDS_NORM1000
                                          ? Str("norm1000") : Str("pixels"));
    }
    ssend(s, &w);
}

// Join a session to the engine: its workspace, and its prefix unless its
// log held one (loadsession), with its job limits.
static void sessionready(Session *s, Arena scratch)
{
    Engine *e = s->e;
    instrstart(s, scratch);
    s->home      = s->ep.name.len ? s->ep.name : s->ep.url;
    s->homemodel = s->model;
    s->guard     = sessionguard(s, &s->perm);
    if (!s->system.data) {
        buildprefix(s, scratch);
    }
    s->next = e->sessions;
    e->sessions = s;

    // Subagents and their parents, as far as both are in memory
    s->parent = s->psid.len ? findsession(e, s->psid) : 0;
    for (Session *t = s->next; t; t = t->next) {
        t->parent = t->psid==s->sid ? s : t->parent;
    }
    arm(s, T_PARK, parkafter(e));
}

// A new session id, unlike any other in the state directory: two made
// in the same second differ only by chance.
static Str freshsid(Engine *e, Arena *a)
{
    Str dir = join(a, e->statedir, "sessions");
    for (i32 i = 0;; i++) {
        Str sid  = newsid(e->os, a);
        Str path = join(a, dir, sid);
        if (i==16 || (os_stat(e->os, *a, path).type==STAT_MISSING &&
                      !findsession(e, sid))) {
            return sid;  // after 16 tries, the random source is broken
        }
    }
}

// Make a new session's directory, lock it and open its log. Returns why
// it could not, or null.
static Str opensession(Session *s, Arena scratch)
{
    Engine *e = s->e;
    if (!os_mkdirs(e->os, scratch, s->outdir)) {
        return "cannot create the session directory";
    }
    Str lock = lockpath(&scratch, s);
    s->lock = os_lockfile(e->os, scratch, lock);
    if (s->lock < 0) {
        return "cannot lock the new session";
    }
    Str path = join(&scratch, s->dir, "events.jsonl");
    s->log = logopen(e->os, &s->perm, scratch, path, s->sid, LOG_MAX);
    if (!s->log.ok) {
        return "cannot open the session log";
    }
    return {};
}

// The session record, always seq 1. A role's prompt and tools are kept
// in it, a subagent's parent, and the edit tool's format unless it is
// replace, so that the session is the same when it is opened again,
// whatever the configuration says then; its prefix record (logprefix),
// after its instructions, keeps the system prompt and tools likewise.
static void logsession(Session *s, Arena scratch)
{
    Buf   b(&scratch, 1024 + s->rolepr.len);
    EvOut w = sbegin(s, &b, EV_SESSION);
    putintkey(&w.json, "proto", 0);
    putstrkey(&w.json, "version", PROSPERO_VERSION);
    putstrkey(&w.json, "cwd", s->cwd);
    putstrkey(&w.json, "role", s->role);
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
    if (s->psid.len) {
        putstrkey(&w.json, "parent", s->psid);
        putstrkey(&w.json, "call", s->pcall);
        putintkey(&w.json, "depth", s->depth);
    }
    if (s->te.deny != TOOL_OFFMASK) {
        putkey(&w.json, "tools");
        putarray(&w.json);
        for (i32 t = 0; t < TOOL_COUNT; t++) {
            if (tooloffered(&s->te, t)) {
                putstr(&w.json, toolnames[t]);
            }
        }
        putend(&w.json);
    }
    if (s->rolepr.len) {
        putstrkey(&w.json, "prompt", s->rolepr);
    }
    if (s->te.edit != EDIT_REPLACE) {
        putstrkey(&w.json, "edit_format", editnames[s->te.edit]);
    }
    if (s->stepmax) {
        putintkey(&w.json, "steps", s->stepmax);
    }
    ssend(s, &w);
}


// Roles and subagents
//
// A role ([role NAME]) is a prompt, a model, an endpoint and a set of
// tools for one kind of work. A session started with one (new {role})
// has the prompt in its system prompt and offers only the role's tools,
// a call to another being an unknown tool's. The roles explore and
// general are built in, as defaults for whatever a section of the same
// name leaves out.
//
// The task tool starts a subagent: a session of its own in this engine,
// whose turn runs the task's prompt under a role (general unless one is
// named), with the role's model, else the parent's. Its requests wait
// their turn at their endpoint with everyone's (admit.cpp). The task
// calls of a response that come in a row start at once (starttasks),
// and end in order as their subagents report (taskflush). A report is
// the subagent's last response: the end of its turn, and of any turns
// its background jobs start by ending while it waits for them. Cancelling
// the parent cancels its subagents. A subagent is attended while its
// parent is: its asks reach the frontend as any session's, and else its
// policy denies them. A subagent may start subagents too, but theirs
// (TASK_DEPTH) get no task tool. At [tools] task_budget requests it is
// told to wrap up, and at half as many again it is stopped, and reports
// what it last said.

struct Role {
    Str name;
    Str desc;
    Str prompt;
    Str model;     // empty for the session's own, or the parent's
    Str endpoint;
    u32 deny;      // tools not offered
    b32 ok;
};

static struct {
    Str name;
    Str desc;
    Str prompt;
    Str tools;     // null for every tool
} builtinroles[] = {
    {
        "explore",
        "Finds and reads code to answer a question, and reports what it "
        "found; it cannot change files or run commands",
        "Explore the code to answer the question you were given: find what "
        "is relevant with grep and glob, read it, and report what you "
        "found, with file paths and line numbers. You can only read: you "
        "cannot change files or run commands.",
        "read grep glob",
    },
    {
        "general",
        "Does any task, with every tool",
        "",
        {},
    },
};

// A role by name: a [role NAME] section's settings, over a built-in
// role's for the keys the section leaves out. Not ok if there is no
// such role. The strings may be the configuration's, until it reloads.
static Role findrole(Engine *e, Str name)
{
    Role r = {};
    i32  b = -1;
    for (i32 i = 0; i < countof(builtinroles); i++) {
        b = builtinroles[i].name==name ? i : b;
    }
    if (!name.len || (b<0 && !inisection(e->ini, "role", name))) {
        return r;
    }
    Config *c     = e->cfg;
    Str     desc  = cfgown(c, "role", name, "description");
    Str     pr    = cfgown(c, "role", name, "prompt");
    Str     tools = cfgown(c, "role", name, "tools");
    if (!tools.data) {
        tools = b>=0 && builtinroles[b].tools.data ? builtinroles[b].tools
                                                   : cfgget(c, "role", name, "tools", 0);
    }
    r.name     = name;
    r.desc     = desc.data ? trim(desc) : b>=0 ? builtinroles[b].desc : Str{};
    r.prompt   = pr.data ? trim(pr) : b>=0 ? builtinroles[b].prompt : Str{};
    r.model    = trim(cfgget(c, "role", name, "model", 0));
    r.endpoint = trim(cfgget(c, "role", name, "endpoint", 0));
    r.deny     = tooldeny(tools);
    r.ok       = 1;
    return r;
}

// The roles a task may give, for the task tool's description and its
// enum: the built-in ones, then the configuration's in its order.
static void taskroles(Engine *e, Arena *perm, Arena scratch, ToolEnv *te)
{
    Slice<Str> names = {};
    for (i32 i = 0; i < countof(builtinroles); i++) {
        names = push(&scratch, names, builtinroles[i].name);
    }
    for (IniSection *s = inifirst(e->ini, "role"); s; s = ininext(s)) {
        b32 seen = !s->name.len;
        for (iz i = 0; i < names.len; i++) {
            seen |= names[i] == s->name;
        }
        if (!seen) {
            names = push(&scratch, names, s->name);
        }
    }

    Buf text(&scratch, 1024);
    for (iz i = 0; i < names.len; i++) {
        Role r = findrole(e, names[i]);
        print(&text, "\n- ");
        print(&text, r.name);
        print(&text, ": ");
        print(&text, r.desc.len ? utf8head(r.desc, 300) : Str("(no description)"));
        if (!(r.deny & ~TOOL_OFFMASK)) {
            print(&text, " (every tool)");
            continue;
        }
        print(&text, " (tools:");
        b32 any = 0;
        for (i32 t = 0; t < TOOL_COUNT; t++) {
            if (!(r.deny>>t & 1)) {
                print(&text, any ? Str(", ") : Str(" "));
                print(&text, toolnames[t]);
                any = 1;
            }
        }
        print(&text, any ? Str(")") : Str(" none)"));
    }
    te->roles = clone(perm, finish(&text));

    Buf     en(&scratch, 256);
    JsonOut w = {&en};
    print(&en, "\"enum\":");
    putarray(&w);
    for (iz i = 0; i < names.len; i++) {
        putstr(&w, names[i]);
    }
    putend(&w);
    te->roleenum = clone(perm, finish(&en));
}

// The session for task call k, its turn not begun. Null if it cannot be
// made, with why in *err, in the parent's arena.
static Session *newchild(Session *p, Arena scratch, iz k, Args *args, Str *err)
{
    Engine *e    = p->e;
    Block  *c    = &p->calls[k];
    Str     name = args->role.len ? args->role : Str("general");
    Role    r    = findrole(e, name);
    if (!r.ok) {
        Buf b(&p->perm, 256);
        print(&b, "No role is named \"");
        print(&b, name);
        print(&b, "\", so no subagent was started. Give one of the roles "
                  "that the task tool lists, or none for general.");
        *err = finish(&b);
        return 0;
    }

    // An explicit model, else the role's, else the parent's own: at the
    // role's endpoint, else the parent's, unless it is ENDPOINT/MODEL
    Str model = args->model.len ? args->model :
                r.model.len     ? r.model     : p->homemodel;
    Str which = r.endpoint.len ? r.endpoint : p->home;
    Cut em    = cut(args->model, '/');
    if (args->model.len && em.ok && em.tail.len &&
            trim(cfgget(e->cfg, "endpoint", em.head, "url", 0)).len) {
        which = em.head;
        model = em.tail;
    }
    Endpoint probe = {};
    if (!endpointconf(e, &scratch, which, &probe)) {
        Buf b(&p->perm, 256);
        print(&b, "No subagent was started: its endpoint, \"");
        print(&b, which);
        print(&b, "\", is neither a URL nor a configured endpoint.");
        *err = finish(&b);
        return 0;
    }

    Str      sid = freshsid(e, &scratch);
    Session *s   = newsession(e, scratch, sid);
    if (!s) {
        *err = "No subagent was started: the harness is out of memory.";
        return 0;
    }
    s->cwd = clone(&s->perm, p->cwd);
    for (iz i = 0; i < p->roots.len; i++) {
        s->roots = push(&s->perm, s->roots, clone(&s->perm, p->roots[i]));
    }
    s->model = clone(&s->perm, model);
    endpointconf(e, &s->perm, which, &s->ep);
    modelconf(e, &s->perm, s->model, &s->mc);
    s->te.edit = s->mc.edit;
    s->role    = clone(&s->perm, r.name);
    s->rolepr  = clone(&s->perm, r.prompt);
    s->depth   = p->depth + 1;
    s->te.deny = r.deny | (s->depth>=TASK_DEPTH ? (u32)1<<TOOL_TASK : 0) |
                 deskdeny(e, 1);
    s->psid    = clone(&s->perm, p->sid);
    s->pcall   = clone(&s->perm, c->id);
    s->intask  = 1;
    // As new asked of the parent, as an eval's
    s->projinstr  = p->projinstr;
    s->nofallback = p->nofallback;
    Str why = opensession(s, scratch);
    if (why.data) {
        dropsession(e, s);
        *err = concat(&p->perm, "No subagent was started: ", why);
        return 0;
    }
    sessionready(s, scratch);
    logsession(s, scratch);
    instrlogstart(s, scratch);
    logprefix(s, scratch);
    // Complete before scratch is passed on, which copies it
    Str title = maketitle(&scratch, args->desc.len ? args->desc : args->prompt);
    settitle(s, scratch, title);
    return s;
}

// Start the subagents of the task calls from the current one on, up to
// the first other call: each in a session of its own, all at once. Each
// call is logged as starting before its subagent exists.
static void starttasks(Session *s, Arena scratch)
{
    Engine *e   = s->e;
    iz      end = s->callidx;
    for (; end<s->calls.len && s->calls[end].name=="task"; end++) {}

    cool(s);  // the next step waits for every report
    s->tasks = {};
    for (iz k = s->callidx; k < end; k++) {
        Arena a = scratch;
        toolstartat(s, a, k);
        TaskRun t = {};
        t.call  = k;
        t.start = os_ticks(e->os);
        Args     args = parseargs(&a, TOOL_TASK, s->calls[k].args, s->te.edit);
        Session *c    = 0;
        if (args.issues.len) {
            t.text = argserror(&s->perm, "task", s->calls[k].args, &args);
        } else if (k-s->callidx >= TASK_BATCH) {
            t.text = "Not started: one response may start at most 16 "
                     "subagents. Start the rest once these have reported.";
        } else {
            c = newchild(s, a, k, &args, &t.text);
        }
        if (c) {
            t.child = clone(&s->perm, c->sid);
        } else {
            t.done   = 1;
            t.status = "error";
        }
        s->tasks = push(&s->perm, s->tasks, t);
        if (c) {
            startturn(c, a, args.prompt, "task");
        }
    }
    setstate(s, S_TASK);
    arm(s, T_TASK, 0);  // calls that started nothing end now
}

// A session's last response with text in it: a subagent's report.
static Str lastreport(Session *s, Arena *a)
{
    for (iz i = s->history.len-1; i >= 0; i--) {
        Msg *m   = &s->history[i];
        b32  any = 0;
        for (iz k = 0; m->role==ROLE_ASSISTANT && k<m->blocks.len; k++) {
            any |= m->blocks[k].type==BLOCK_TEXT && trim(m->blocks[k].text).len;
        }
        if (!any) {
            continue;
        }
        Buf b(a, 1024);
        for (iz k = 0; k < m->blocks.len; k++) {
            if (m->blocks[k].type == BLOCK_TEXT) {
                print(&b, m->blocks[k].text);
            }
        }
        return trim(finish(&b));
    }
    return {};
}

// A subagent is done with its task: its report goes to the parent's
// call, which ends once the calls before it have (taskflush). why and
// err are as its last turn ended.
static void taskreport(Session *c, Arena scratch, Str why, Str err)
{
    c->intask = 0;
    Session *p = c->parent;
    TaskRun *t = 0;
    for (iz i = 0; p && i<p->tasks.len; i++) {
        t = p->tasks[i].child==c->sid ? &p->tasks[i] : t;
    }
    if (!t || t->done) {
        return;  // its parent stopped waiting (cancelled, or quit)
    }

    Str report = lastreport(c, &scratch);
    Buf b(&scratch, 512 + report.len + err.len);
    t->status = "ok";
    if (why == "done") {
        print(&b, report.len ? report : Str("The subagent ended its turn "
                                            "without a report."));
    } else if (why == "cancel") {
        t->status = "cancelled";
        print(&b, "The subagent was cancelled before it finished.");
    } else if (why == "budget") {
        t->status = "error";
        print(&b, "The subagent was stopped before it finished, at the end "
                  "of its budget of ");
        print(&b, c->treqs - 1);
        print(&b, " model requests.");
    } else {
        t->status = "error";
        print(&b, "The subagent stopped before it finished, on an error: ");
        print(&b, err.len ? err : why);
    }
    if (why!="done" && report.len) {
        print(&b, " What it said last:\n\n");
        print(&b, report);
    }
    Str text = finish(&b);

    if (text.len > TASK_REPORT) {
        // All of it in a file the agent may read, and the start inline
        Engine *e    = p->e;
        Str     rel  = {};
        Str     path = spillfile(p, scratch, p->calls[t->call].id, &rel);
        Str     head = utf8head(text, TASK_REPORT);
        Buf     in(&scratch, head.len + 256 + path.len);
        print(&in, head);
        if (os_write(e->os, scratch, path, text)) {
            t->spill = rel;
            t->bytes = text.len;
            print(&in, "\n\n[The report goes on: it was cut here at 50 KiB. "
                       "All of it is in ");
            print(&in, path);
            print(&in, ", where this part ends at line ");
            print(&in, countlines(head));
            print(&in, ".]");
        } else {
            print(&in, "\n\n[The report goes on, but it was cut here at "
                       "50 KiB.]");
        }
        text = finish(&in);
    }
    t->text = clone(&p->perm, text);
    t->done = 1;
    arm(p, T_TASK, 0);
    live(p);  // one fewer at work
}

// A subagent's turn ended: its task did too, unless it ended well with
// background jobs running, whose ends will start the next turn. Once it
// reports, any jobs it left are stopped.
static void taskcheck(Session *s, Arena scratch, Str why, Str err)
{
    if (!s->intask || busy(s)) {
        return;
    } else if (why=="done" && jobsrunning(s) && canresume(s)) {
        return;
    }
    stopjobs(s, scratch, s->e->quitting);
    taskreport(s, scratch, why, err);
}

// End the task calls whose subagents have reported, in order, and once
// they all have, go on with the calls after them.
static void taskflush(Session *s, Arena scratch)
{
    if (s->state != S_TASK) {
        return;
    }
    while (s->callidx < s->calls.len) {
        iz i = s->callidx - s->tasks[0].call;
        if (i >= s->tasks.len || !s->tasks[i].done) {
            break;
        }
        Arena    a = scratch;
        TaskRun *t = &s->tasks[i];
        s->callstart = t->start;
        toolend(s, a, t->status, t->text, 0, 0, t->spill, t->bytes, 0, t->child);
    }
    for (iz i = 0; i < s->tasks.len; i++) {
        if (!s->tasks[i].done) {
            return;
        }
    }
    s->tasks = {};
    setstate(s, S_IDLE);
    nexttool(s, scratch);
}

// Cancel the subagents still at work: each reports at once, and their
// calls end as they do.
static void taskcancel(Session *s, Arena scratch)
{
    Engine *e = s->e;
    for (iz i = 0; i < s->tasks.len; i++) {
        Session *c = s->tasks[i].done ? 0 : findsession(e, s->tasks[i].child);
        if (!c) {
            continue;
        }
        Arena a = scratch;
        cancelturn(c, a);
        if (c->intask && !busy(c)) {
            // It waited for its background jobs
            stopjobs(c, a, e->quitting);
            taskreport(c, a, "cancel", {});
        }
    }
}

// Count a subagent's request against its budget: a notice to wrap up at
// [tools] task_budget, then at half as many again the end of its turn.
// Returns 0 if the turn ended.
static b32 taskbudget(Session *s, Arena scratch)
{
    i64 soft = confint(s->e->cfg, "tools", "", "task_budget");
    soft = soft ? soft : TASK_SOFT;
    i64 hard = soft + soft/2;
    if (++s->treqs > hard) {
        Buf b(&scratch, 128);
        print(&b, "the subagent reached its budget of ");
        print(&b, hard);
        print(&b, " model requests");
        Str err = finish(&b);
        endturn(s, scratch, "budget", err);
        return 0;
    } else if (s->treqs == soft) {
        Buf b(&scratch, 512);
        print(&b, "You have made ");
        print(&b, soft);
        print(&b, " model requests on this task, its budget. Wrap up now: "
                  "start nothing new, and give your final report, with what "
                  "you did, what you found, and what is left undone. After ");
        print(&b, hard - soft);
        print(&b, " more requests you will be stopped, and only what you "
                  "said last reaches the agent that is waiting for you.");
        notify(s, "budget", finish(&b), 0);
    }
    return 1;
}


// Idle parking
//
// A session idle for PARK_IDLE (7 minutes) gives up its memory: it is
// forgotten but for what its log does not hold (Parked), still holding
// its lock, and the next command that names it revives it from its log.
// One with something going on in the background, a job, a notice to
// deliver or a report to make, is not parked.

static Session *loadsession(Engine *, Arena, Str sid, Str model, Str which,
                            i64 *firstnew, iz held);

static i64 parkafter(Engine *e)
{
    return scaled(e, PARK_IDLE);
}

static b32 parkable(Session *s)
{
    for (Job *j = s->jobs; j; j = j->next) {
        if (j->state != JOB_DONE) {
            return 0;
        }
    }
    return !busy(s) && !s->intask && !s->notices.len && !s->queued.len &&
           !s->steers.len && !s->http && !s->ctl && s->slot==SLOT_NONE &&
           !s->e->quitting;
}

static void park(Engine *e, Session *s)
{
    Parked k = {};
    k.sid      = clone(&e->perm, s->sid);
    k.model    = clone(&e->perm, s->homemodel);
    k.which    = clone(&e->perm, s->home);
    k.lock     = s->lock;
    k.attended = s->attended;
    k.closed   = s->closed;
    e->parked  = push(&e->perm, e->parked, k);
    s->lock    = -1;  // kept for its revival
    for (Session *t = e->sessions; t; t = t->next) {
        t->parent = t->parent==s ? 0 : t->parent;
    }
    dropsession(e, s);
}

// A session closed with forget may go once nothing runs in it, whatever
// it would have told the agent next: nobody will open it again here.
static b32 forgettable(Session *s)
{
    for (Job *j = s->jobs; j; j = j->next) {
        if (j->state != JOB_DONE) {
            return 0;
        }
    }
    return !busy(s) && !s->intask && !s->http && !s->ctl && s->slot==SLOT_NONE;
}

// Drop a session from memory for good, its lock released. Its log stays,
// for an open to read.
static void forget(Engine *e, Session *s)
{
    for (Session *t = e->sessions; t; t = t->next) {
        t->parent = t->parent==s ? 0 : t->parent;
    }
    dropsession(e, s);
}

static iz parkedat(Engine *e, Str sid)
{
    for (iz i = 0; i < e->parked.len; i++) {
        if (e->parked[i].sid == sid) {
            return i;
        }
    }
    return -1;
}

// Bring a parked session back from its log, as it was, or with another
// model or endpoint if given. Null if it is not parked, or its log is
// gone. *firstnew is as loadsession has it.
static Session *revive(Engine *e, Arena scratch, Str sid, Str model,
                       Str which, i64 *firstnew)
{
    iz at = parkedat(e, sid);
    if (at < 0) {
        return 0;
    }
    Parked k = e->parked[at];
    e->parked[at] = e->parked[e->parked.len-1];
    e->parked.len--;
    model = model.len ? model : k.model;
    which = which.len ? which : k.which;
    Session *s = loadsession(e, scratch, sid, model, which, firstnew, k.lock);
    if (s) {
        s->attended = k.attended;
        s->closed   = k.closed;
    }
    return s;
}

// Asks that nobody can answer any more, as a frontend let go of a
// session or an ancestor of it: denied by policy.
static void unattend(Engine *e, Arena scratch)
{
    for (Session *t = e->sessions; t; t = t->next) {
        if (t->state==S_ASK && !attending(t)) {
            Arena a = scratch;
            resolve(t, a, 0, DENY_POLICY, "the session is no longer attended");
            nexttool(t, a);
        }
    }
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

// The first word of a tool list that names no tool, or null.
static Str unknowntools(Str list)
{
    for (Str rest = list; rest.len;) {
        iz i = 0;
        for (; i<rest.len && (whitespace(rest[i]) || rest[i]==','); i++) {}
        rest = cuthead(rest, i);
        for (i = 0; i<rest.len && !whitespace(rest[i]) && rest[i]!=','; i++) {}
        Str word = takehead(rest, i);
        if (word.len && toolfind(word)==TOOL_NONE) {
            return word;
        }
        rest = cuthead(rest, i);
    }
    return {};
}

// new {cwd, model?, endpoint?, role?, roots?, attended?, prompt?,
//      edit_format?, tools?, steps?, instructions?, fallback?}: the model
// and endpoint given, else the role's, else the configuration's. The
// rest are for runs that must not depend on the configuration, as an
// eval's: the edit format instead of the model's, only the tools listed
// (of the role's, if any), at most steps model requests a turn (up to
// 300), with instructions "project", none of the user's own instruction
// file, and with fallback 0 (or false), none of the model's fallbacks,
// so that every request goes to the model given. Its subagents keep the
// last two.
static void cmdnew(Engine *e, Arena scratch, i64 id, Json *j)
{
    Str  cwd   = jsonstr(jsonget(j, "cwd"));
    Str  model = jsonstr(jsonget(j, "model"));
    Str  which = jsonstr(jsonget(j, "endpoint"));
    Str  name  = jsonstr(jsonget(j, "role"));
    Str  fmt   = trim(jsonstr(jsonget(j, "edit_format")));
    Str  tools = jsonstr(jsonget(j, "tools"));
    i64  steps = jsonint(jsonget(j, "steps"), 0);
    Str  instr = jsonstr(jsonget(j, "instructions"));
    b32  fall  = jsonflag(jsonget(j, "fallback"), 1);
    Role role  = name.len ? findrole(e, name) : Role{};
    if (name.len && !role.ok) {
        Str err = concat(&scratch, "no such role: ", name);
        reply(e, scratch, id, 0, err, {});
        return;
    }
    if (fmt.len && !equalfold(editnames[editformat(fmt)], fmt)) {
        Str err = concat(&scratch, "no such edit format: ", fmt);
        reply(e, scratch, id, 0, err, {});
        return;
    }
    Str bad = unknowntools(tools);
    if (bad.len) {
        Str err = concat(&scratch, "no such tool: ", bad);
        reply(e, scratch, id, 0, err, {});
        return;
    } else if (tools.data && !trim(tools).len) {
        reply(e, scratch, id, 0, "tools lists no tool", {});
        return;
    } else if (steps<0 || steps>STEP_HARD) {
        reply(e, scratch, id, 0, "steps must be 1 to 300", {});
        return;
    } else if (instr.len && instr!="all" && instr!="project") {
        reply(e, scratch, id, 0, "instructions must be all or project", {});
        return;
    }
    if (!which.len) which = role.endpoint;
    if (!model.len) model = role.model;
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

    Str      sid = freshsid(e, &scratch);
    Session *s   = newsession(e, scratch, sid);
    if (!s) {
        reply(e, scratch, id, 0, "out of memory", {});
        return;
    }
    s->cwd        = clone(&s->perm, real);
    s->model      = clone(&s->perm, model);
    s->attended   = jsonflag(jsonget(j, "attended"), 0);
    s->role       = clone(&s->perm, role.name);
    s->rolepr     = clone(&s->perm, role.prompt);
    s->te.deny    = role.deny | (tools.data ? tooldeny(tools) : 0) |
                    deskdeny(e, role.ok);
    s->stepmax    = steps<STEP_HARD ? steps : 0;
    s->projinstr  = instr == "project";
    s->nofallback = !fall;
    for (iz i = 0; i < roots.len; i++) {
        s->roots = push(&s->perm, s->roots, clone(&s->perm, roots[i]));
    }
    endpointconf(e, &s->perm, which, &s->ep);
    modelconf(e, &s->perm, model, &s->mc);
    s->te.edit = fmt.len ? editformat(fmt) : s->mc.edit;

    Str why = opensession(s, scratch);
    if (why.data) {
        dropsession(e, s);
        reply(e, scratch, id, 0, why, {});
        return;
    }
    sessionready(s, scratch);
    logsession(s, scratch);
    instrlogstart(s, scratch);
    logprefix(s, scratch);
    reply(e, scratch, id, 1, {}, s->sid);

    Str prompt = jsonstr(jsonget(j, "prompt"));
    if (prompt.len) {
        startturn(s, scratch, prompt, "prompt");
    }
}

// What the tools keep of a session, from one record of its log: where
// calls started, jobs, the todo list, snapshots, instruction files, and
// the notices the agent read.
static void replaytools(Session *s, Arena scratch, EvRec *r)
{
    Json *j    = r->root;
    Str   call = jsonstr(jsonget(j, "call"));
    switch (r->ev) {
    case EV_TOOLSTART: {
        CallMark k = {clone(&s->perm, call), r->seq, s->turn};
        s->marks = push(&s->perm, s->marks, k);
    } break;

    case EV_JOBSTART: {
        Job *jb = alloc<Job>(&s->perm);
        jb->n       = jsonint(jsonget(j, "job"), 0);
        jb->call    = clone(&s->perm, call);
        jb->command = clone(&s->perm, jsonstr(jsonget(j, "command")));
        jb->rel     = clone(&s->perm, jsonstr(jsonget(j, "spill")));
        jb->spill   = clone(&s->perm, join(&scratch, s->dir, jb->rel));
        jb->state   = JOB_RUN;  // until its end
        jb->quiet   = 1;        // nothing to tell before its end
        jb->next    = s->jobs;
        s->jobs     = jb;
        s->njobs    = jb->n>s->njobs ? jb->n : s->njobs;
    } break;

    case EV_JOBEND: {
        Job *jb = jobfind(s, jsonint(jsonget(j, "job"), 0));
        if (!jb) break;
        Str st = jsonstr(jsonget(j, "status"));
        jb->state  = JOB_DONE;
        jb->why    = JOBEND_LOST;
        for (i32 k = 0; k < JOBEND_COUNT; k++) {
            jb->why = jobendnames[k]==st ? k : jb->why;
        }
        jb->code   = (i32)jsonint(jsonget(j, "exit"), 0);
        jb->signal = (i32)jsonint(jsonget(j, "signal"), 0);
        jb->total  = jsonint(jsonget(j, "bytes"), 0);
        jb->ran    = jsonint(jsonget(j, "ms"), 0) * MS;
        jb->quiet  = !jsonflag(jsonget(j, "notice"), 1);
    } break;

    case EV_NOTICE: {
        Msg m = {};
        m.role = ROLE_USER;
        m.text = clone(&s->perm, jsonstr(jsonget(j, "text")));
        m.seq  = r->seq;  // a later compaction's cut keeps it or not
        addmsg(s, m);
        Job *jb = jobfind(s, jsonint(jsonget(j, "job"), 0));
        if (jb && jsonstr(jsonget(j, "kind"))=="job") {
            jb->quiet = 1;
        }
    } break;

    case EV_TODO: {
        s->todos = {};
        for (Json *k = jsonget(j, "items")->child; k; k = k->next) {
            TodoItem t = {};
            t.content = clone(&s->perm, jsonstr(jsonget(k, "content")));
            t.status  = todostatus(jsonstr(jsonget(k, "status")));
            t.status  = t.status<0 ? TODO_PENDING : t.status;
            s->todos  = push(&s->perm, s->todos, t);
        }
    } break;

    case EV_SNAP: {
        Snap sn = {};
        Str  name = jsonstr(jsonget(j, "snap"));
        sn.n       = parseint(cuthead(name, startswith(name, "snap/") ? 5 : 0)).val;
        sn.seq     = r->seq;
        sn.turn    = s->turn;
        sn.call    = clone(&s->perm, call);
        sn.path    = clone(&s->perm, jsonstr(jsonget(j, "path")));
        sn.before  = clone(&s->perm, jsonstr(jsonget(j, "hash_before")));
        sn.after   = clone(&s->perm, jsonstr(jsonget(j, "hash_after")));
        sn.existed = jsonflag(jsonget(j, "existed"), 0);
        s->snaps   = push(&s->perm, s->snaps, sn);
        s->nsnap   = sn.n>s->nsnap ? sn.n : s->nsnap;
    } break;

    case EV_REWIND:
        for (Json *k = jsonget(j, "undone")->child; k; k = k->next) {
            Str name = jsonstr(k);
            for (iz i = 0; i < s->snaps.len; i++) {
                if (snapname(&scratch, s->snaps[i].n) == name) {
                    s->snaps[i].undone = 1;
                }
            }
        }
        break;

    case EV_INSTRUCTIONS: {
        Str why = jsonstr(jsonget(j, "why"));
        if (why == "start") {
            s->instr = clone(&s->perm, jsonstr(jsonget(j, "text")));
            s->instr.data = s->instr.data ? s->instr.data : (u8 *)"";
        }
        for (Json *k = jsonget(j, "files")->child; k; k = k->next) {
            Str         path = jsonstr(jsonget(k, "path"));
            InstrStamp *at   = 0;
            for (iz i = 0; i < s->instrfiles.len; i++) {
                at = s->instrfiles[i].path==path ? &s->instrfiles[i] : at;
            }
            if (!at) {
                InstrStamp n = {clone(&s->perm, path), 0, 0};
                s->instrfiles = push(&s->perm, s->instrfiles, n);
                at = &s->instrfiles[s->instrfiles.len-1];
            }
            at->size  = jsonint(jsonget(k, "bytes"), 0);
            at->mtime = jsonint(jsonget(k, "mtime"), 0);
        }
    } break;
    }
}

// After replaying a log: jobs whose end the agent never heard of are
// told at its next request. Any still open were in another process, and
// are over for this one.
static void replayjobs(Session *s, Arena scratch)
{
    Slice<Job *> untold = {};
    for (Job *j = s->jobs; j; j = j->next) {
        if (j->state != JOB_DONE) {
            j->state = JOB_DONE;
            j->why   = JOBEND_LOST;
            j->quiet = 0;
        }
        if (!j->quiet) {
            untold = push(&scratch, untold, j);
        }
    }
    for (iz i = untold.len-1; i >= 0; i--) {  // oldest first
        Job  *j  = untold[i];
        Arena a  = scratch;
        Str  text = jobnotice(s, &a, j, j->total);
        notify(s, "job", text, j->n);
        j->quiet = 1;
    }
}

// Rebuild a session from its log: the history, the turn count, and the
// closing of anything a crash left open (logopen), including results for
// calls that never started. *firstnew is the seq before any repairs.
// When another process holds the session's lock, it is only read, and
// so neither repaired nor written later (readonly). A parked session's
// lock is held already: held, else -1.
static Session *loadsession(Engine *e, Arena scratch, Str sid, Str model,
                            Str which, i64 *firstnew, iz held)
{
    Str      dir  = join(&scratch, join(&scratch, e->statedir, "sessions"), sid);
    Str      path = join(&scratch, dir, "events.jsonl");
    Session *s    = 0;
    if (os_stat(e->os, scratch, path).type != STAT_FILE ||
            !(s = newsession(e, scratch, sid))) {
        if (held >= 0) {
            os_unlockfile(e->os, held);
        }
        return 0;
    }
    if (held >= 0) {
        s->lock = held;
    } else {
        Str lock = lockpath(&scratch, s);
        s->lock = os_lockfile(e->os, scratch, lock);
    }
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
    b32 prefixed    = 0;
    for (iz i = 0; i < rp.lines.len; i++) {
        Arena t = scratch;
        EvRec r = evparse(&t, rp.lines[i]);
        Json *j = r.root;
        Msg   m = {};
        switch (r.ev) {
        case EV_SESSION: {
            s->cwd      = clone(&s->perm, jsonstr(jsonget(j, "cwd")));
            storedmodel = clone(&s->perm, jsonstr(jsonget(j, "model")));
            storedurl   = clone(&s->perm, jsonstr(jsonget(j, "endpoint")));
            for (Json *k = jsonget(j, "roots")->child; k; k = k->next) {
                Str root = clone(&s->perm, jsonstr(k));
                s->roots = push(&s->perm, s->roots, root);
            }
            // Its role and place among subagents, as it began
            s->role   = clone(&s->perm, jsonstr(jsonget(j, "role")));
            s->rolepr = clone(&s->perm, jsonstr(jsonget(j, "prompt")));
            s->psid   = clone(&s->perm, jsonstr(jsonget(j, "parent")));
            s->pcall  = clone(&s->perm, jsonstr(jsonget(j, "call")));
            i64 depth = jsonint(jsonget(j, "depth"), 0);
            s->depth  = (i32)(depth<0 ? 0 : depth>TASK_DEPTH ? TASK_DEPTH : depth);
            Json *tools = jsonget(j, "tools");
            if (tools->type == JSON_ARRAY) {
                u32 have = 0;
                for (Json *k = tools->child; k; k = k->next) {
                    i32 tool = toolfind(jsonstr(k));
                    have |= tool==TOOL_NONE ? 0 : (u32)1<<tool;
                }
                s->te.deny = (((u32)1<<TOOL_COUNT) - 1) & ~have;
            } else {
                s->te.deny = TOOL_OFFMASK;  // as it began: every tool but
            }                               // those offered by name
            // Its edit format as it began: absent, the first one
            s->te.edit = editformat(jsonstr(jsonget(j, "edit_format")));
            i64 steps  = jsonint(jsonget(j, "steps"), 0);
            s->stepmax = steps>0 && steps<STEP_HARD ? steps : 0;
        } break;
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
            s->thinkfrom = r.seq;
        } break;
        case EV_ASSISTANT:
            m.seq  = r.seq;
            m.role = ROLE_ASSISTANT;
            for (Json *k = jsonget(j, "blocks")->child; k; k = k->next) {
                Block b = {};
                b.type  = blocktype(jsonstr(jsonget(k, "t")));
                Str txt = b.type==BLOCK_REDACTED ? Str("data") : Str("text");
                b.text  = clone(&s->perm, jsonstr(jsonget(k, txt)));
                b.sig   = clone(&s->perm, jsonstr(jsonget(k, "sig")));
                b.id    = clone(&s->perm, jsonstr(jsonget(k, "id")));
                b.name  = clone(&s->perm, jsonstr(jsonget(k, "name")));
                b.args  = clone(&s->perm, jsonstr(jsonget(k, "args")));
                m.blocks = push(&s->perm, m.blocks, b);
            }
            if (jsonstr(jsonget(j, "finish")) != "refusal") {
                addmsg(s, m);  // a declined one is never sent back (finalize)
            }
            {
                // The estimates as the server last calibrated them
                Json *u     = jsonget(j, "usage");
                i64   in    = jsonint(jsonget(u, "in"), 0);
                i64   bytes = jsonint(jsonget(u, "bytes"), 0);
                if (in>0 && bytes>0) {
                    s->rate.tokens = in;
                    s->rate.bytes  = bytes;
                }
            }
            break;
        case EV_TOOLEND:
            m.role   = ROLE_TOOL;
            m.callid = clone(&s->perm, jsonstr(jsonget(j, "call")));
            m.text   = clone(&s->perm, jsonstr(jsonget(j, "text")));
            m.spill  = clone(&s->perm, jsonstr(jsonget(j, "spill")));
            m.error  = toolfailed(jsonstr(jsonget(j, "status")));
            m.seq    = r.seq;
            m.image  = clone(&s->perm, jsonstr(jsonget(j, "image")));
            m.imgw   = (i32)jsonint(jsonget(j, "width"), 0);
            m.imgh   = (i32)jsonint(jsonget(j, "height"), 0);
            if (m.image.len) {
                // Points refer to the latest screenshot, as they did
                DeskShot sh = deskshotparse(&s->perm, jsonstr(jsonget(j, "shot")));
                s->desk.shot = sh.ok ? sh : s->desk.shot;
            }
            addmsg(s, m);
            break;
        case EV_PREFIX:
            // As it began, rather than as the configuration has it now
            s->system        = clone(&s->perm, jsonstr(jsonget(j, "system")));
            s->tools         = clone(&s->perm, jsonstr(jsonget(j, "tools")));
            s->te.jobtimeout = jsonint(jsonget(j, "job_timeout"), 0);
            s->te.jobmax     = jsonint(jsonget(j, "job_timeout_max"), 0);
            s->te.coords     = jsonstr(jsonget(j, "coordinates"))=="norm1000"
                               ? COORDS_NORM1000 : COORDS_PIXELS;
            s->system.data   = s->system.data ? s->system.data : (u8 *)"";
            prefixed         = 1;
            break;
        }
        replaytools(s, t, &r);
    }
    if (prefixed && (s->te.jobtimeout<1 || s->te.jobmax<1)) {
        jobconf(s);  // a record without them
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
    replayjobs(s, scratch);
    sessionready(s, scratch);
    instrlogstart(s, scratch);
    if (!prefixed) {
        logprefix(s, scratch);  // a log from before prefix records
    }
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
        Str        kind = {};
        Str        text = {};
        PartCall  *c    = 0;
        PartBlock *pb   = 0;
        for (iz n = 0; n < p->parts.len; n++) {
            pb = p->parts[n].block==i ? &p->parts[n] : pb;
        }
        if (i == p->thinkblock) {
            kind = "think";
            text = finish(&p->think);
        } else if (i == p->textblock) {
            kind = "text";
            text = finish(&p->text);
        } else if (pb && pb->type==BLOCK_REDACTED) {
            continue;
        } else if (pb) {
            kind = pb->type==BLOCK_THINK ? Str("think") : Str("text");
            text = finish(&pb->text);
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
        Str model = jsonstr(jsonget(j, "model"));
        Str which = jsonstr(jsonget(j, "endpoint"));
        s = parkedat(e, sid)>=0 ? revive(e, scratch, sid, model, which, &first)
                                : loadsession(e, scratch, sid, model, which, &first, -1);
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
        unattend(e, scratch);  // nobody is left to answer it, or its subagents
    }
    sendsnapshot(s, scratch);
    s->closed = 0;

    Buf   b(&scratch, 256);
    EvOut w = evbegin(&b, s->sid, 0, os_time(e->os), EV_REPLY);
    putintkey(&w.json, "id", id);
    putintkey(&w.json, "ok", 1);
    putintkey(&w.json, "readonly", s->readonly);
    emit(e, evend(&w));
    live(s);  // the state of a session that was open already
}

struct RewindFile {
    Str path;
    Str result;  // restored, removed, unchanged, conflict, partial, failed
    b32 wrote;   // this rewind changed the file
};

// rewind {sid, to, force}: put back, newest first, the files that write
// and edit changed at or after a call (to: its id) or a turn (to: its
// number), unless something else changed them since, which is a
// conflict, left alone unless forced. Logs a rewind record and tells
// the agent at its next request.
static void cmdrewind(Engine *e, Session *s, Arena scratch, i64 id, Json *j)
{
    Os  *os    = e->os;
    Json *to   = jsonget(j, "to");
    b32  force = jsonflag(jsonget(j, "force"), 0);
    i64  seq0  = -1;   // snaps after this record
    i64  turn0 = -1;   // or from this turn on
    Str  label = {};
    if (busy(s)) {
        reply(e, scratch, id, 0, "the session is busy: cancel its turn first", s->sid);
        return;
    } else if (to->type == JSON_STRING) {
        // Newest first, as frontends find a call, should an old log have
        // a model's id more than once
        for (iz i = s->marks.len-1; i>=0 && seq0<0; i--) {
            seq0 = s->marks[i].call==to->str ? s->marks[i].seq : seq0;
        }
        if (seq0 < 0) {
            reply(e, scratch, id, 0, "no call with that id in this session", s->sid);
            return;
        }
        label = concat(&scratch, "before call ", to->str);
    } else if (to->type == JSON_NUMBER) {
        turn0 = jsonint(to, -1);
        if (turn0<1 || turn0>s->turn) {
            reply(e, scratch, id, 0, "no turn with that number in this session", s->sid);
            return;
        }
        Buf b(&scratch, 32);
        print(&b, "to the start of turn ");
        print(&b, turn0);
        label = finish(&b);
    } else {
        reply(e, scratch, id, 0, "to must be a call id or a turn number", s->sid);
        return;
    }

    Slice<RewindFile> files  = {};
    Slice<Str>        undone = {};
    for (iz i = s->snaps.len-1; i >= 0; i--) {
        Snap *sn = &s->snaps[i];
        if (sn->undone || (turn0>=0 ? sn->turn<turn0 : sn->seq<=seq0)) {
            continue;
        }
        // One result for each file: its oldest change decides, unless a
        // newer one found a conflict or failed, which stops it there. A
        // conflict after newer changes were put back is a partial rewind.
        RewindFile *f = 0;
        for (iz k = 0; k<files.len && !f; k++) {
            f = files[k].path==sn->path ? &files[k] : 0;
        }
        if (f && (f->result=="conflict" || f->result=="partial" ||
                  f->result=="failed")) {
            continue;
        }
        Arena a   = scratch;
        Read  cur = os_read(os, &a, sn->path, (iz)FILE_MAX);
        Str   now = cur.err==READ_NOTFOUND ? Str("") :
                    cur.err                ? Str("?") : hashstr(&a, hash(cur.data));
        Str   result = {};
        if (now == sn->before) {
            result = "unchanged";  // as it was already
        } else if (!force && now!=sn->after) {
            result = "conflict";
        } else if (sn->existed) {
            Str  name = snapname(&a, sn->n);
            Str  file = join(&a, s->dir, name);
            Read old  = os_read(os, &a, file, (iz)FILE_MAX);
            Str  dir  = normpath(&a, sn->path, "..", s->te.windows);
            b32  ok   = !old.err;
            if (ok && os_stat(os, a, dir).type!=STAT_DIR) {
                ok = os_mkdirs(os, a, dir);
            }
            ok = ok && os_write(os, a, sn->path, old.data);
            result = ok ? Str("restored") : Str("failed");
        } else {
            b32 ok = cur.err==READ_NOTFOUND || os_remove(os, a, sn->path);
            result = ok ? Str("removed") : Str("failed");
        }
        b32 wrote = result=="restored" || result=="removed";
        if (result!="conflict" && result!="failed") {
            sn->undone = 1;
            undone = push(&scratch, undone, snapname(&scratch, sn->n));
        }
        if (!f) {
            RewindFile n = {sn->path, result, wrote};
            files = push(&scratch, files, n);
        } else if (f->wrote && (result=="conflict" || result=="failed")) {
            f->result = "partial";
        } else {
            f->result = result;
            f->wrote |= wrote;
        }
    }

    Str note = "Changes made by shell commands are not tracked, so they "
               "were not undone.";
    if (files.len) {
        Buf   b(&scratch, 1024);
        EvOut w = sbegin(s, &b, EV_REWIND);
        putkey(&w.json, "to");
        putjson(&w.json, to);
        putintkey(&w.json, "force", force);
        putkey(&w.json, "files");
        putarray(&w.json);
        for (iz i = 0; i < files.len; i++) {
            putobject(&w.json);
            putstrkey(&w.json, "path", files[i].path);
            putstrkey(&w.json, "result", files[i].result);
            putend(&w.json);
        }
        putend(&w.json);
        putkey(&w.json, "undone");
        putarray(&w.json);
        for (iz i = 0; i < undone.len; i++) {
            putstr(&w.json, undone[i]);
        }
        putend(&w.json);
        putstrkey(&w.json, "note", note);
        ssend(s, &w);

        // What the agent reads at its next request
        static struct {
            Str result;
            Str head;
        } const groups[] = {
            {"restored",  "Put back as they were: "},
            {"removed",   "Removed, since they did not exist then: "},
            {"unchanged", "Already as they were: "},
            {"conflict",  "Left as they are, since something else changed "
                          "them after the harness did: "},
            {"partial",   "Put back only to before the later changes, since "
                          "something else changed them in between, so the "
                          "earlier changes were kept: "},
            {"failed",    "Could not be put back: "},
        };
        Buf t(&scratch, 1024);
        print(&t, "The user rewound the files the harness changed, ");
        print(&t, label);
        print(&t, ".");
        for (iz g = 0; g < countof(groups); g++) {
            iz n = 0;
            for (iz i = 0; i < files.len; i++) {
                if (files[i].result != groups[g].result) continue;
                print(&t, n++ ? Str(", ") : Str("\n"));
                if (n == 1) print(&t, groups[g].head);
                print(&t, showpath(s->guard.cwd, files[i].path));
            }
        }
        print(&t, "\nChanges made by shell commands were not undone. Read "
                  "files again before you edit them.");
        Str text = finish(&t);
        notify(s, "rewind", text, 0);
    } else {
        note = "Nothing to rewind: write and edit changed no files at or "
               "after that point. Changes made by shell commands are not "
               "tracked.";
    }

    Buf   b(&scratch, 1024);
    EvOut w = evbegin(&b, s->sid, 0, os_time(e->os), EV_REPLY);
    putintkey(&w.json, "id", id);
    putintkey(&w.json, "ok", 1);
    putkey(&w.json, "files");
    putarray(&w.json);
    for (iz i = 0; i < files.len; i++) {
        putobject(&w.json);
        putstrkey(&w.json, "path", files[i].path);
        putstrkey(&w.json, "result", files[i].result);
        putend(&w.json);
    }
    putend(&w.json);
    putstrkey(&w.json, "note", note);
    emit(e, evend(&w));
}

// file {sid, path, off?, max?, encoding?}: part of a file in the session's
// directory, for a frontend that cannot read it there, as on another
// machine: a spill file, out/N.log. The path is relative, and it and
// what it resolves to stay inside the directory. From off (0), up to max
// bytes, or FILECMD_MAX, or less where the frontend's mailbox could not
// take the reply. The reply has the text, cleaned as tool output is and
// cut on a UTF-8 boundary, off where the next part begins, the file's
// size, and more:1 if there is more after it. With encoding "base64",
// for a binary file such as a screenshot, the part comes as is in
// base64, as data rather than text, and cut anywhere.
static void cmdfile(Engine *e, Session *s, Arena scratch, i64 id, Json *j)
{
    Os *os     = e->os;
    Str path   = jsonstr(jsonget(j, "path"));
    i64 off    = jsonint(jsonget(j, "off"), 0);
    i64 max    = jsonint(jsonget(j, "max"), FILECMD_MAX);
    b32 binary = jsonstr(jsonget(j, "encoding")) == "base64";
    // Every byte may be escaped, or base64 takes 4 for 3
    iz  most   = mailmax(e->out) / (binary ? 2 : 8);
    most = most<(iz)FILECMD_MAX ? most : (iz)FILECMD_MAX;
    max  = max<1 || max>most ? most : max;

    // Plain names below the directory: no root, drive, "..", or NUL
    b32 bad = !path.len || off<0 || path[0]=='/' || path[0]=='\\' ||
              cut(path, 0).ok || cut(path, ':').ok;
    for (Str rest = path; !bad && rest.len;) {
        iz i = 0;
        for (; i<rest.len && rest[i]!='/' && rest[i]!='\\'; i++) {}
        bad  = takehead(rest, i) == "..";
        rest = cuthead(rest, i + (i<rest.len));
    }
    Str dir  = bad ? Str{} : os_realpath(os, &scratch, s->dir);
    Str real = dir.data ? os_realpath(os, &scratch, join(&scratch, s->dir, path)) : Str{};
    if (!real.data || !within(normpath(&scratch, dir, ".", HOST_WINDOWS),
                              normpath(&scratch, dir, real, HOST_WINDOWS), HOST_WINDOWS)) {
        reply(e, scratch, id, 0, "no such file in the session's directory", s->sid);
        return;
    }
    Read rd = os_readat(os, &scratch, real, off, (iz)max);
    if (rd.err != READ_OK) {
        reply(e, scratch, id, 0, rd.err==READ_NOTFOUND ? Str("no such file")
                                                       : Str("cannot read the file"), s->sid);
        return;
    }
    Str data = rd.data;
    b32 more = off+data.len < rd.size;
    if (more && !binary) {
        // Before any character at the end, which may be cut short
        iz i = data.len - 1;
        for (; i>0 && data.len-i<4 && (data[i]&0xc0)==0x80; i--) {}
        data.len = i>0 && data[i]>=0x80 ? i : data.len;
    }

    Str   text = binary ? base64(&scratch, data) : cleanoutput(&scratch, data);
    Buf   b(&scratch, 256 + 6*text.len);
    EvOut w = evbegin(&b, s->sid, 0, os_time(os), EV_REPLY);
    putintkey(&w.json, "id", id);
    putintkey(&w.json, "ok", 1);
    putstrkey(&w.json, "path", path);
    putstrkey(&w.json, binary ? Str("data") : Str("text"), text);
    putintkey(&w.json, "off", off + data.len);
    putintkey(&w.json, "size", rd.size);
    putintkey(&w.json, "more", more);
    emit(e, evend(&w));
}

// A session as the sessions list shows it.
struct ListEntry {
    Str       sid;
    Str       title;
    Str       cwd;
    Str       model;
    Str       parent;  // a subagent's, else empty
    i64       mtime;
    b32       locked;  // open in another process
    b32       parked;
    b32       shown;
    Slice<iz> kids;    // its subagents, oldest first
};

static b32 listbefore(ListEntry *a, ListEntry *b)
{
    return compare(a->sid, b->sid) < 0;
}

// An entry, then its subagents under it, each with its depth there.
static void listput(Engine *e, JsonOut *w, Slice<ListEntry> list, iz i, i32 depth)
{
    ListEntry *en = &list[i];
    if (en->shown) {
        return;
    }
    en->shown = 1;
    Session *live = findsession(e, en->sid);
    Str      state = live ? statenames[live->state] : en->parked ? Str("idle")
                                                                 : Str("closed");
    putobject(w);
    putstrkey(w, "sid", en->sid);
    putstrkey(w, "title", en->title);
    putstrkey(w, "cwd", en->cwd);
    putstrkey(w, "model", en->model);
    putstrkey(w, "state", state);
    putintkey(w, "mtime", en->mtime);
    putintkey(w, "locked", en->locked);
    putintkey(w, "jobs", live ? jobsrunning(live) : 0);
    if (en->parent.len) {
        putstrkey(w, "parent", en->parent);
        putintkey(w, "depth", depth);
    }
    if (en->parked) {
        putintkey(w, "parked", 1);
    }
    putend(w);
    for (iz k = 0; k<en->kids.len && depth<=TASK_DEPTH+8; k++) {
        listput(e, w, list, en->kids[k], depth+1);
    }
}

// The sessions in the state directory, newest first, but a subagent's
// under its parent, oldest first: in the order a sidebar shows them.
static void cmdlist(Engine *e, Arena scratch, i64 id)
{
    Slice<ListEntry> list = {};
    Str dir = join(&scratch, e->statedir, "sessions");
    for (Dirent *d = os_list(e->os, &scratch, dir); d; d = d->next) {
        if (!d->isdir) continue;
        Str   sdir = join(&scratch, dir, d->name);
        Str   path = join(&scratch, sdir, "events.jsonl");
        Read  rd   = os_readat(e->os, &scratch, path, 0, 4096);
        Cut   line = cut(rd.data, '\n');
        EvRec r    = evparse(&scratch, line.head);
        if (!line.ok || r.ev!=EV_SESSION) continue;
        Session  *s     = findsession(e, d->name);
        Str       title = join(&scratch, sdir, "title");
        Str       lock  = join(&scratch, sdir, "lock");
        ListEntry en    = {};
        en.sid    = d->name;
        en.title  = os_read(e->os, &scratch, title, 1024).data;
        en.cwd    = jsonstr(jsonget(r.root, "cwd"));
        en.model  = jsonstr(jsonget(r.root, "model"));
        en.parent = jsonstr(jsonget(r.root, "parent"));
        en.mtime  = d->mtime;
        en.parked = !s && parkedat(e, d->name)>=0;  // its lock is ours
        en.locked = (!s || s->readonly) && !en.parked &&
                    os_lockheld(e->os, scratch, lock);
        list = push(&scratch, list, en);
    }

    // Each subagent under its parent, if listed
    sort(list.data, list.len, listbefore, scratch);
    Map<iz> *bysid = 0;
    for (iz i = 0; i < list.len; i++) {
        *upsert(&bysid, list[i].sid, &scratch) = i;
    }
    for (iz i = 0; i < list.len; i++) {
        iz *p = list[i].parent.len ? upsert(&bysid, list[i].parent, (Arena *)0) : 0;
        if (p && *p!=i) {
            list[*p].kids = push(&scratch, list[*p].kids, i);
        } else {
            list[i].parent = {};  // shown as any session
        }
    }

    Buf   b(&scratch, 4096);
    EvOut w = evbegin(&b, {}, 0, os_time(e->os), EV_SESSIONS);
    putkey(&w.json, "list");
    putarray(&w.json);
    for (iz i = list.len-1; i >= 0; i--) {
        if (!list[i].parent.len) {
            listput(e, &w.json, list, i, 0);
        }
    }
    for (iz i = list.len-1; i >= 0; i--) {
        listput(e, &w.json, list, i, 0);  // in a cycle of parents
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

// Reply to config.get or config.set with the configuration as loaded.
static void replyconfig(Engine *e, Arena scratch, i64 id, i32 cmd, CfgSet *r)
{
    Config *c = e->cfg;
    Buf     b(&scratch, 1<<14);
    EvOut   w = evbegin(&b, {}, 0, os_time(e->os), EV_REPLY);
    putintkey(&w.json, "id", id);
    putintkey(&w.json, "ok", r->ok);
    if (!r->ok) {
        putstrkey(&w.json, "err", r->err);
    }
    if (cmd == CMD_CONFIGGET) {
        putcfg(&w.json, c);
    } else {
        putcfgset(&w.json, c, r);
    }
    emit(e, evend(&w));
}

// Answer the config.get {refresh} commands waiting for fetches, once
// none is under way or due.
static void configwaiters(Engine *e, Arena scratch)
{
    if (!e->ncfgwait || cfgbusy(e->cfg)) {
        return;
    }
    for (i32 i = 0; i < e->ncfgwait; i++) {
        CfgSet r = {};
        r.ok = 1;
        replyconfig(e, scratch, e->cfgwait[i], CMD_CONFIGGET, &r);
    }
    e->ncfgwait = 0;
}

// A record of a URL include's fetch (config.cpp). Returns 0 if it is
// not one.
static b32 onconfig(Engine *e, Arena scratch, Record r)
{
    i64 rev = e->cfg->rev;
    if (!cfgrecord(e->cfg, scratch, r)) {
        return 0;
    }
    if (e->cfg->rev != rev) {
        reconfigure(e);
    }
    configwaiters(e, scratch);
    return 1;
}

// config.get and config.set, with the shapes in config.cpp. With
// refresh, config.get fetches every URL include anew, and its reply
// waits for the fetches to end (30 s at most), unless 8 such replies
// wait already, when it comes at once.
static void cmdconfig(Engine *e, Arena scratch, i64 id, i32 cmd, Json *j)
{
    Config *c   = e->cfg;
    CfgSet  r   = {};
    i64     rev = c->rev;
    if (cmd == CMD_CONFIGGET) {
        r.ok = 1;
        cfgpoll(c, scratch, 1);
        if (jsonflag(jsonget(j, "refresh"), 0)) {
            cfgrefresh(c, scratch, 1);
            if (cfgbusy(c) && e->ncfgwait<countof(e->cfgwait)) {
                e->cfgwait[e->ncfgwait++] = id;
                if (c->rev != rev) {
                    reconfigure(e);
                }
                return;
            }
        }
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
    replyconfig(e, scratch, id, cmd, &r);
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
        case CMD_REWIND:
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
        putstrkey(&w.json, "version", PROSPERO_VERSION);
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
            stopjobs(t, scratch, 1);
        }
        reply(e, scratch, id, 1, {}, {});
    } return;
    case CMD_CONFIGGET:
    case CMD_CONFIGSET:
        cmdconfig(e, scratch, id, cmd, j);
        return;
    case CMD_STATUS: {
        // What runs: turns (asks and queued requests included) and
        // commands, those being stopped too, over every open session
        i64 open = 0, turns = 0, jobs = 0;
        for (Session *t = e->sessions; t; t = t->next) {
            open++;
            turns += busy(t);
            for (Job *jb = t->jobs; jb; jb = jb->next) {
                jobs += jb->state != JOB_DONE;
            }
        }
        Arena a = scratch;
        Buf   b(&a, 256);
        EvOut w = evbegin(&b, {}, 0, os_time(e->os), EV_REPLY);
        putintkey(&w.json, "id", id);
        putintkey(&w.json, "ok", 1);
        putintkey(&w.json, "open", open);
        putintkey(&w.json, "parked", e->parked.len);
        putintkey(&w.json, "turns", turns);
        putintkey(&w.json, "jobs", jobs);
        emit(e, evend(&w));
    } return;
    case CMD_STOP:
        // As quit does, but running on: a background host's "Stop all"
        for (Session *t = e->sessions; t; t = t->next) {
            cancelturn(t, scratch);
            stopjobs(t, scratch, 0);
        }
        reply(e, scratch, id, 1, {}, {});
        return;
    case CMD_NONE:
        reply(e, scratch, id, 0, "unknown command", {});
        return;
    }

    if (!s && sid.len && !e->quitting) {
        i64 first = 0;
        s = revive(e, scratch, sid, {}, {}, &first);  // a parked session
    }
    if (!s) {
        reply(e, scratch, id, 0, "no such open session", sid);
        return;
    } else if (s->readonly && cmd!=CMD_CLOSE && cmd!=CMD_FILE) {
        reply(e, scratch, id, 0, "read-only: another process has this "
              "session open; open it again once that one has closed it", sid);
        return;
    }
    switch (cmd) {
    case CMD_FILE:
        cmdfile(e, s, scratch, id, j);
        return;
    case CMD_CLOSE:
        // Nobody is left to answer asks, nor to need background jobs.
        // With forget, the session leaves memory, its lock released, as
        // soon as nothing goes on in it (enginethread): an eval's runs
        // are many, and none is opened again.
        reply(e, scratch, id, 1, {}, sid);
        s->attended = 0;
        s->forget   = jsonflag(jsonget(j, "forget"), 0);
        if (!s->readonly) {
            s->closed = 1;
            stopjobs(s, scratch, 0);
        }
        if (s->state==S_ASK && !attending(s)) {
            resolve(s, scratch, 0, DENY_POLICY, "the user closed the session");
            nexttool(s, scratch);
        }
        unattend(e, scratch);  // its subagents' asks
        return;
    case CMD_REWIND:
        cmdrewind(e, s, scratch, id, j);
        return;
    case CMD_JOBKILL: {
        Job *jb = jobfind(s, jsonint(jsonget(j, "job"), 0));
        if (!jb) {
            reply(e, scratch, id, 0, "no such job in this session", sid);
            return;
        }
        reply(e, scratch, id, 1, {}, sid);
        jobstop(s, jb, JOBEND_KILLED);  // the agent is told as it ends
    } return;
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
    } else if (onconfig(e, scratch, r) || onprobe(e, scratch, r) ||
               onctl(e, scratch, r)) {
        return;
    }

    b32 proc = r.type>=PROC_TAIL && r.type<=PROC_DATA;
    if (proc) {
        Session *ds = deskbyid(e, r.id);
        if (ds) {
            deskrecord(ds, scratch, r);
            return;
        }
        Job     *j  = 0;
        Session *js = jobbyproc(e, r.id, &j);
        if (js) {
            jobrecord(js, scratch, j, r);
            return;
        }
    }
    Session *s = sessionbyid(e, r.id, proc);
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
        if (s->sh.tool != TOOL_SHELL) {
            // A search out of time: keep what it found
            s->sh.timedout = 1;
            os_kill(e->os, s->proc, 1);
            arm(s, T_DRAIN, DRAIN);
            return;
        } else if (s->sh.exited) {
            return;  // done, and draining its output already
        } else if (jobsrunning(s) < JOB_RUNNING) {
            movetojob(s, scratch);
            nexttool(s, scratch);
            return;
        }
        // No room for another job: stopped like a cancelled command
        s->sh.timedout = 1;
        s->sh.nojob    = 1;
        os_kill(e->os, s->proc, 0);
        arm(s, T_KILL, stopgrace(e));
        return;
    case T_KILL:
        os_kill(e->os, s->proc, 1);
        return;
    case T_JOBS:
        jobtick(s, scratch);
        return;
    case T_DRAIN:
        finishshell(s, scratch, 0);
        nexttool(s, scratch);
        return;
    case T_ASK:
        resolve(s, scratch, 0, DENY_TIMEOUT, {});
        nexttool(s, scratch);
        return;
    case T_DESK:
        deskbroke(s, scratch, "The desktop helper did not answer in time, so "
                  "it was stopped, and the action may not have been done.");
        return;
    case T_LIVE:
        live(s);
        arm(s, T_LIVE, LIVE_EVERY);
        return;
    case T_TASK:
        taskflush(s, scratch);
        return;
    case T_PARK:
        if (busy(s)) {
            return;  // its turn's end arms it again
        } else if (!parkable(s)) {
            arm(s, T_PARK, parkafter(e));
        } else {
            s->parkme = 1;  // parked outside the loop over sessions
        }
        return;
    }
}

static void enginethread(void *arg)
{
    Engine *e = (Engine *)arg;
    cfgrefresh(e->cfg, newarena(e->scratch), 0);  // what loaded from a cache
    for (;;) {
        i64 now  = os_ticks(e->os);
        i64 next = e->quitting ? e->quitby : I64MAX;
        next = e->admat && e->admat<next ? e->admat : next;
        next = cfgnext(e->cfg)<next ? cfgnext(e->cfg) : next;
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
        if (cfgnext(e->cfg) <= now) {
            i64 rev = e->cfg->rev;
            cfgtimeout(e->cfg, newarena(e->scratch));
            if (e->cfg->rev != rev) {
                reconfigure(e);
            }
            configwaiters(e, newarena(e->scratch));
        }
        for (Session **p = &e->sessions; *p;) {
            Session *s = *p;
            if (s->forget && forgettable(s)) {
                forget(e, s);  // unlinks it: *p is the next
            } else if (s->parkme && parkable(s)) {
                park(e, s);
            } else {
                s->parkme = 0;
                p = &s->next;
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
            for (iz i = 0; i < e->parked.len; i++) {
                if (e->parked[i].lock >= 0) {
                    os_unlockfile(e->os, e->parked[i].lock);
                }
            }
            e->parked = {};
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

// An executable on PATH, by its file name, or null.
static Str findpath(Os *os, Arena *perm, Arena scratch, Str name, b32 win)
{
    Str path = os_getenv(os, &scratch, "PATH");
    for (Str rest = path; rest.len;) {
        Cut c = cut(rest, win ? ';' : ':');
        rest = c.tail;
        if (!c.head.len) {
            continue;
        }
        Arena a = scratch;
        Str   p = join(&a, c.head, name);
        if (os_stat(os, a, p).type == STAT_FILE) {
            return clone(perm, p);
        }
    }
    return {};
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
    Region *r   = os_reserve(os, (iz)1<<(big ? 32 : 27));
    e->scratch  = os_reserve(os, (iz)1<<(big ? 32 : 27));
    e->replay   = os_reserve(os, (iz)1<<(big ? 32 : 27));
    if (!e->in || !r || !e->scratch || !e->replay) {
        return 0;
    }
    e->perm    = newarena(r);
    Str exe    = os_exepath(os, &e->perm);
    e->exedir  = parentdir(exe);
    e->te      = findshell(os, &e->perm, newarena(e->scratch), e->exedir, HOST_WINDOWS);
    e->te.exe  = exe;
    e->env     = os_environ(os, &e->perm);
    e->home    = os_getenv(os, &e->perm, "HOME");
    if (HOST_WINDOWS && !e->home.len) {
        e->home = os_getenv(os, &e->perm, "USERPROFILE");
    }
    if (cfgbool(trim(cfgget(cfg, "tools", "", "use_rg", 0))) != 0) {
        Str rg = HOST_WINDOWS ? Str("rg.exe") : Str("rg");
        e->te.rg = findpath(os, &e->perm, newarena(e->scratch), rg, HOST_WINDOWS);
    }
    // The user's own instructions, next to the configuration
    Str conf = cfgdefault(os, &e->perm);
    if (conf.len) {
        e->global = normpath(&e->perm, parentdir(conf), "AGENTS.md", HOST_WINDOWS);
    }
    loadtrained(e, newarena(e->scratch));
    // Tests shorten minutes of slow retries to milliseconds
    Int minute = parseint(os_getenv(os, &e->perm, "PROSPERO_TEST_MINUTE_MS"));
    e->minute  = minute.ok && minute.val>0 && minute.val<=60000 ? minute.val*MS : 60*SEC;
    // URL includes refresh in the background now, every ten minutes
    cfgattach(cfg, e->in, 10*e->minute);
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
