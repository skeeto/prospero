// Session view model: one session's transcript and state, built only
// from protocol v0 records
//
// The GUI never reads engine memory. It applies event records, the same
// lines the session log holds, to its own model of each session, and
// draws from that. Streamed deltas grow provisional messages, which the
// committed assistant record later replaces. Tool calls become rows that
// tool.start, tool.out and tool.end fill in, and asks become cards that
// answer resolves. Transient records keep the liveness state: what the
// session is doing, how fast, and how long since the model sent data.
//
// Durable records are applied once each, in seq order: a record already
// applied is ignored, and one that skips ahead means records were lost
// (a full mailbox), so the view asks for a resync from the log (gap),
// which applies the rest in order. Replays may skip numbers, as logs do
// around damaged lines, so while one runs (loose) any later seq goes.
// A view that went missing records says so (lost) until resynced.
// Deltas of an attempt already committed are stale and ignored, which
// covers deltas that arrive after a resync read their commit.
//
// Memory, by lifetime:
//   text   message sources, appended forever; the streaming message is
//          the last allocation, so its deltas extend it in place
//   msgs   the message array, alone in its arena so it grows in place
//   aux    tool rows and asks, their details and output tails
//   cache  parsed documents and layouts, shared by every view and reset
//          wholesale when full: a generation number marks everything
//          made before as stale
//
// Messages have heights, measured once laid out or else estimated from
// their size, and each knows its top: the prefix sum of the heights
// above it, recomputed lazily at most once per frame, which a binary
// search turns into the range of messages on screen.
//
// A selection is a pair of positions (message, source offset), ordered
// lexicographically, so it spans messages and survives relayout and
// streaming. Copying turns it back into text or markdown.
//
// Reasoning shows as the [ui] thinking setting says (viewthink): folded
// to its header, even while it streams (fold, the default), open, or
// not at all (hide), when it takes no space and draws nothing. A row
// the user opened or closed keeps that (chosen) through setting
// changes, and through its commit: a commit, or a snapshot, carries the
// state of the provisional reasoning it replaces over to its own, by
// their place among the response's reasoning.
//
// Tool rows fold as [ui] tools says (viewtools): auto, the default, folds
// reads, writes and todo lists, and once a call ends opens a failure and
// folds a background job's start and a long shell output; fold and open
// apply to every row. A row the user opened or closed keeps that, through
// its end too. A session's own toggles (viewshowtools, viewshowthink)
// open every row of their kind, hidden reasoning included, until turned
// off, leaving the configuration alone.
//
// A search (ViewSearch) finds text in the messages' sources, ignoring
// ASCII case, folded rows included but not hidden reasoning. It keeps up
// with the view a budget of bytes at a time, searching again only from
// the first message that changed, which while streaming is at the end.
//
// A change of width keeps what is at the top edge there (viewanchor,
// anchory): the message, the source offset its line there begins at, and
// how far into that line the edge is, found in the old layout and put
// back in the new one.
//
// The context gauge's numbers come from records that carry them: the
// tokens the context holds, where it compacts and the model's window
// (ctx, compact_at, tokens on attempt, turn.end and live), and a response's
// usage, its tokens and how many of them the server had cached.
//
// Besides its transcript a view keeps what the panels beside it show:
// the agent's todo list, the session's background jobs with the latest
// of their output, the files write and edit changed (snapshots), which
// a rewind puts back, and the instruction files loaded. Compactions and
// rewinds are cards in the transcript, and notices the agent read are
// folded notes, so that the user sees what the agent was told.
//
// This is free and unencumbered software released into the public domain.

enum : i32 {
    VIEW_USER,
    VIEW_ASSISTANT,
    VIEW_THINK,
    VIEW_TOOL,
    VIEW_NOTE,
    VIEW_ASK,
    VIEW_COMPACT,  // a compaction's card
    VIEW_REWIND,   // a rewind's card
    VIEW_MARK,     // a change to the session, one line: its label, then text
};

// How tool rows fold ([ui] tools)
enum : i32 {
    TOOLS_AUTO,  // by kind and outcome
    TOOLS_FOLD,
    TOOLS_OPEN,
    TOOLS_COUNT,
};

static Str toolsnames[] = {"auto", "fold", "open"};
static_assert(countof(toolsnames) == TOOLS_COUNT);

// The setting's value from its name, or -1 if it names none.
static i32 toolsparse(Str s)
{
    for (i32 i = 0; i < TOOLS_COUNT; i++) {
        if (toolsnames[i] == s) {
            return i;
        }
    }
    return -1;
}

// Tool rows
enum : i32 {
    VTOOL_CALLED,     // committed, not yet started
    VTOOL_STREAMING,  // provisional: its arguments are arriving
    VTOOL_ASKING,     // waiting for the user's answer
    VTOOL_RUNNING,
    VTOOL_OK,
    VTOOL_ERROR,
    VTOOL_DENIED,
    VTOOL_TIMEOUT,
    VTOOL_CANCELLED,
    VTOOL_UNKNOWN,
    VTOOL_BACKGROUND,  // a command moved to the background, as a job
    VTOOL_COUNT,
};

static Str vtoolnames[] = {
    "called", "streaming", "asking", "running", "ok", "error", "denied",
    "timeout", "cancelled", "unknown", "background",
};
static_assert(countof(vtoolnames) == VTOOL_COUNT);

// Background jobs, as job.end has them
enum : i32 {
    VJOB_RUNNING,
    VJOB_EXITED,
    VJOB_TIMEOUT,
    VJOB_KILLED,
    VJOB_CLOSED,
    VJOB_LIMIT,
    VJOB_UNKNOWN,
    VJOB_FORKED,  // it ran on in the session this one is a fork of
    VJOB_COUNT,
};

static Str vjobnames[] = {
    "running", "exited", "timeout", "killed", "closed", "limit", "unknown",
    "forked",
};
static_assert(countof(vjobnames) == VJOB_COUNT);

// Todo items
enum : i32 {
    VTODO_PENDING,
    VTODO_ACTIVE,
    VTODO_DONE,
};

// Asks
enum : i32 {
    VASK_PENDING,
    VASK_ALLOWED,
    VASK_DENIED,
};

// How a turn ended, as a desktop notification tells of it (endwhy):
// the ends the session came to by itself, and the rest
enum : i32 {
    END_OTHER,   // cancelled by the user, a side chat's start, a crash
    END_DONE,
    END_ERROR,
    END_BUDGET,
};

// What a session is doing, from live records ("closed": not loaded)
enum : i32 {
    LIVE_IDLE,
    LIVE_DISCOVER,
    LIVE_WAITING,
    LIVE_STREAMING,
    LIVE_ERROR,      // reading an error response
    LIVE_BACKOFF,
    LIVE_TOOL,
    LIVE_ASK,
    LIVE_QUEUED,      // waiting for its turn at the endpoint
    LIVE_TASK,        // waiting for its subagents
    LIVE_MCP,         // waiting for its MCP servers to start
    LIVE_COMPACTING,  // a summary request runs
    LIVE_CLOSED,
    LIVE_COUNT,
};

static Str livenames[] = {
    "idle", "discover", "waiting", "streaming", "error", "backoff", "tool",
    "ask", "queued", "task", "mcp", "compacting", "closed",
};
static_assert(countof(livenames) == LIVE_COUNT);

// How reasoning shows ([ui] thinking)
enum : i32 {
    THINK_FOLD,  // its header only, until opened
    THINK_OPEN,
    THINK_HIDE,  // not at all
    THINK_COUNT,
};

static Str thinknames[] = {"fold", "open", "hide"};
static_assert(countof(thinknames) == THINK_COUNT);

// The setting's value from its name, or -1 if it names none.
static i32 thinkparse(Str s)
{
    for (i32 i = 0; i < THINK_COUNT; i++) {
        if (thinknames[i] == s) {
            return i;
        }
    }
    return -1;
}

enum : iz {
    VIEW_DIFFMAX = 64 << 10,  // bytes of a write's content shown as a diff
    VIEW_SUMMARY = 160,       // bytes of a tool row's summary
};

// A tool call's row, or an ask's card.
struct ViewTool {
    Str  call;      // call id
    Str  name;      // tool name
    Str  args;      // raw arguments (JSON)
    Str  summary;   // one line: the command, the path
    i32  block;     // provisional: the response block its arguments are
    i32  status;    // VTOOL_*
    i32  exit;
    b32  hasexit;
    i64  start;     // epoch ms of tool.start
    i64  startseq;  // tool.start's record, which a rewind to it follows
    i64  ms;        // its duration, once ended
    Str  spill;     // the full output, relative to the session directory
    i64  bytes;
    b32  ended;     // tool.end came
    i64  outbytes;  // output so far, while running
    u8  *tail;      // the live output, a buffer reused for each tool.out
    iz   tailcap;
    i64  job;       // the background job it started, 0 if none
    Str  child;     // a task's subagent, once it reported, or a
                    // message's target
    Str  host;      // the host a task's subagent runs on, if not this one
    b32  opened;    // the window reached that subagent (kidsweep), once
    b32  want;      // the user asked to see it: shown once reached
    i32  snap;      // the file it changed: index+1 in the view's snaps
    b32  elided;    // pruned: the agent sees only where its output is
    Str  image;     // a screenshot, relative to the session directory
    i32  imgw;      // its size in pixels
    i32  imgh;
    // Asks
    Str  req;
    Str  kind;      // path, desktop, plan (detail the plan), plan.enter
    Str  tool;      // the tool that asked
    Str  path;
    Str  detail;
    Str  hint;
    i64  deadline;  // epoch ms, 0 for none (a plan's)
    i32  answer;    // VASK_*
    Str  grant;     // what always would allow: a directory or a file,
                    // or a desktop class (scope "desktop"), or an MCP
                    // server's tools (mcp__SERVER) or one tool's
    Str  access;    // and for what: read or write
    Str  scope;     // "desktop" for a class, else a path
    b32  always;    // allowed for good (a grant)
    Str  trust;     // an MCP project server that may be trusted for good
    b32  trusted;   // and it was
    Str  by;
    Str  note;
};

// A compaction (kind "summary" or "prune") or a rewind, as a card.
struct ViewCard {
    Str  kind;       // summary, prune, rewind
    Str  why;        // threshold or overflow
    i64  before;     // tokens, estimated
    i64  after;
    Str  model;      // the summary's
    i64  ms;
    i32  count;      // messages summarized, outputs elided, files rewound
    i32  conflicts;  // files a rewind left alone
    Str  to;         // a rewind's point: a call id, or else
    i32  turn;       // the start of this turn
    b32  force;
    b32  latest;     // the latest rewind, which may be forced
};

// A background job (shell with background, or moved there at its timeout).
struct ViewJob {
    i64  n;
    Str  call;
    Str  command;
    Str  summary;   // its first line
    Str  spill;     // the full output, relative to the session directory
    i64  start;     // epoch ms
    i64  deadline;  // epoch ms: the job limit stops it then
    i32  status;    // VJOB_*
    i32  exit;
    b32  hasexit;
    i32  signal;
    i64  bytes;
    i64  ms;        // how long it ran, once ended
    u8  *tail;      // the latest output, a buffer reused for each job.out
    iz   taillen;
    iz   tailcap;
    b32  killing;   // a kill was asked for
};

struct ViewTodo {
    Str content;
    i32 status;     // VTODO_*
};

// A message sent to the turn at work, waiting to be taken in (queue).
struct ViewQueued {
    Str text;       // cut to its start if long: unqueue gives it whole
    i64 id;
    b32 steer;      // for the next step, else a prompt for a turn of its own
    b32 cut;        // text is only its start
    i32 images;     // attached
};

// A file that write or edit changed, whose original a rewind restores.
struct ViewSnap {
    Str  call;
    Str  path;      // absolute
    Str  name;      // "snap/N", as a rewind's undone list names it
    i64  seq;
    i32  turn;
    b32  existed;   // else a rewind removes it
    b32  undone;
};

// An instruction file the agent was given.
struct ViewInstr {
    Str why;        // start, nested, changed
    Str path;
    i64 bytes;
    Str importer;   // the file that imported it, if any
};

// A memory in the project's index, MEMORY.md: its line's link and words.
struct ViewMem {
    Str name;
    Str desc;
};

// An image the user attached to a message, in the session directory.
struct ViewImage {
    Str name;  // img/N.png or img/N.jpg
    Str mime;
    i32 w;
    i32 h;
};

struct ViewMsg {
    Str       text;       // source, in the text arena
    i32       role;
    i32       turn;
    i32       attempt;    // the response it came from, 0 if none
    i64       seq;        // durable record, or 0 while provisional
    i32       ver;        // bumped when text or shape changes
    i64       nl;         // newlines in text, for estimates
    b32       prov;       // provisional: streamed, not yet committed
    b32       faded;      // provisional text of a failed attempt
    b32       collapsed;  // only the header shows
    b32       chosen;     // the user opened or closed it: settings keep off
    b32       fold;       // its header shows and hides the rest
    b32       md;         // markdown, not plain text
    i32       startsturn; // the turn this message began, 0 if none
    i64       chars;      // reasoning: code points in its text
    i64       ts0;        // reasoning: its first and last deltas, epoch
    i64       ts1;        //   ms, or 0 where it did not stream
    Str       label;      // the header, if not the role's name
    Str       from;       // a message's sender, a session's id, or the
                          // endpoint's URL of a mark of a model, or the
    b32       fromkid;    // subagent whose report it is (kind task)
    Str       fromhost;   // and the host it ran on, if not this one
    ViewTool *tool;       // tool rows and asks
    ViewCard *card;       // compactions and rewinds
    Slice<ViewImage> images;  // the user's, attached
    f64       top;        // prefix sum of heights above, with gaps
    f32       h;          // content height: measured, or estimated
    b32       measured;   // h is from a layout at the current width
    // Cached in the shared cache, valid while gen matches
    Doc      *doc;
    i32       docver;
    i32       docgen;
    MsgLayout layout;
    i32       layoutver;
    i32       layoutgen;
    f32       layoutw;
};

struct Pos {
    i32 msg;
    i32 off;
};

enum : i32 {
    SEL_CHAR,
    SEL_WORD,
    SEL_BLOCK,
};

struct Sel {
    Pos anchor;   // where the selection began
    Pos anchor1;  // with word or block units: the end of the first unit
    Pos focus;    // where it extends to
    i32 unit;     // SEL_*
    b32 active;
};

// Documents and layouts of every view, reset together when full.
struct ViewCache {
    Arena        arena;
    byte        *base;
    iz           max;    // reset past this many bytes
    i32          gen;
    HlMemo      *memo;
    LayoutCache *lcache;
};

struct View {
    Arena           text;
    Arena           meta;
    Arena           aux;
    ViewCache      *c;
    Str             dir;        // the session's directory, for spill paths
    Slice<ViewMsg>  msgs;
    i32             stream;     // the provisional message, -1 if none
    i32             streamkind; // its delta kind: VIEW_ASSISTANT or VIEW_THINK
    i32             prov;       // first provisional message, -1 if none
    byte           *provtext;   // text arena position when it began
    b32             provmixed;  // others were added after it began
    b32             topsdirty;
    f32             gap;        // between messages
    f32             extra;      // header and padding around each message
    f32             foot;       // an open ask's buttons
    f32             thumbw;     // the box a screenshot's thumbnail fits
    f32             thumbh;
    f32             thumbpad;   // and the space around it
    f32             thumbrow;   // the width a user's thumbnails wrap at
    Sel             sel;
    i64             deltas;     // delta records applied, for rates
    i64             deltabytes;
    i64             laidout;    // messages laid out, for benchmarks
    b32             overflow;   // fake agent: events were dropped
    i32             thinking;   // THINK_*: how reasoning shows
    i32             toolmode;   // TOOLS_*: how tool rows fold
    b32             showtools;  // the session's toggles: every tool row
    b32             showthink;  //   or reasoning open, whatever the mode
    f32             lastw;      // the width laid out last
    f32             head;       // a message's header line, as drawn

    // Ordering
    i64             lastseq;    // the last durable record applied
    i64             logoff;     // where the last resync's reading ended
    b32             lost;       // records went missing: resync from the log
    b32             loose;      // a replay, or a host's: seq may skip numbers
    i32             doneturn;   // the last committed response:
    i32             doneattempt;  // deltas up to it are stale

    // The session
    Str             title;
    Str             cwd;
    b32             nofolder;   // cwd is a folder of the session's own
    Str             model;
    Str             endpoint;
    Str             shell;      // the shell commands run in, by name
    Str             shellpath;  // and its program
    b32             shellset;   // by a shell record, not the prefix
    Str             role;
    Str             parent;     // a subagent's parent session, else empty
    Str             pcall;      // and the parent's task call
    b32             remote;     // that parent is on another host
    i32             depth;
    i32             hostcalls;  // task calls with a host, for kidsweep
    Str             tools;      // those offered, if restricted: blank separated
    Str             roleprompt;
    Str             scratch;    // its scratch directory, if any
    Str             forkedfrom; // the session it was forked from, if any
    i64             forkturn;   // and the turn it was cut after
    Str             sideof;     // a side chat's original session, else empty
    i32             turn;
    i32             attempt;
    b32             busy;       // a turn is running
    b32             novision;   // its model takes no images (live)
    b32             readonly;   // open elsewhere: answering is not possible
    i64             turnts;     // epoch ms the running turn began
    i32             asks;       // pending asks
    b32             plan;       // plan mode
    b32             bypass;     // Bypass mode: nothing asked but plans
    Str             planpath;   // the plan file, once in plan mode
    i32             planver;    // bumped as the plan file may change

    // Liveness, from live and progress records (epoch ms)
    i32             state;      // LIVE_*
    i64             lastdata;   // the model last sent anything
    f64             tps;
    i64             retryat;    // backing off until then
    i64             watchat;    // the nearest watchdog fires then, 0 if none
    i64             watchms;    // and waits this long in all
    i64             pdone;      // prompt processing, tokens of total
    i64             ptotal;
    i64             ctx;        // the context gauge: the model's window,
    i64             compactat;  //   where compaction begins,
    i64             tokens;     //   the tokens it holds now,
    i64             cached;     //   and of the last prompt, those cached
    i64             outts;      // the last tool.out
    i32             queuepos;   // queued: the place in line, of
    i32             queuelen;
    i32             tasks;      // subagents still at work
    Str             retrymodel; // the next attempt's model, from a retry
    b32             retryslow;  // minutes-long waits
    i32             fallback;   // switched to this fallback (1-based)

    // Beside the transcript
    Slice<ViewTodo>  todos;     // the latest list
    Slice<ViewQueued> queue;    // the latest queue record's
    Slice<ViewJob>   jobs;      // by number
    i32              jobsrun;   // running
    Slice<ViewSnap>  snaps;     // in order
    Slice<ViewInstr> instr;     // instruction files, by path
    Slice<Str>       skipped;   // instruction files not loaded
    Str              instrtext; // what went into the system prompt
    Str              memdir;    // the project's memory, empty if none
    Slice<ViewMem>   mems;      // its index, as the agent knows it
    i64              cutseq;    // records before it are summarized

    // What a desktop notification may be about (app.cpp, toastpass): the
    // latest ask and turn end, by their records, which a replay writes
    // again the same
    i64             askseq;      // the last ask's record
    i64             askts;
    i32             askmsg;      // and its message
    i64             endseq;      // the last turn.end's record
    i64             endts;
    i32             endwhy;      // END_*
    Str             enderr;
    b32             endresume;   // the next turn began by itself
    b32             endcompact;  // a compaction's turn (compact) ended
    b32             turncompact; // the running turn is one
};

static ViewCache *newviewcache(Arena *perm, Region *r, iz max)
{
    ViewCache *c = alloc<ViewCache>(perm);
    c->arena = newarena(r);
    c->base  = c->arena.beg;
    c->max   = max;
    return c;
}

static View *newview(Arena *perm, ViewCache *c, Region *text, Region *meta,
                     Region *aux)
{
    View *v = alloc<View>(perm);
    v->c         = c;
    v->text      = newarena(text);
    v->meta      = newarena(meta);
    v->aux       = newarena(aux);
    v->stream    = -1;
    v->prov      = -1;
    v->topsdirty = 1;
    v->state     = LIVE_CLOSED;
    return v;
}

// At frame start: drop every cached document and layout if the cache
// has grown past its budget. Nothing from the cache may be held across
// this call. Measured heights stay right: layouts are only made again.
static void cachereset(ViewCache *c, b32 force)
{
    if (force || c->arena.beg-c->base > c->max) {
        c->arena.beg = c->base;
        c->gen++;
        c->memo   = 0;
        c->lcache = 0;
    }
}

// Newlines in s, eight bytes at a time: a history's texts are megabytes,
// and debug builds are slow at them a byte at a time. Under the xor a
// newline is a zero byte, and exactly the zero bytes keep a high bit.
static i64 countnl(Str s)
{
    u64 lo = 0x7f7f7f7f7f7f7f7fu;
    i64 n  = 0;
    iz  i  = 0;
    for (; s.len-i >= 8; i += 8) {
        u64 w = 0;
        __builtin_memcpy(&w, s.data+i, 8);
        w ^= 0x0a0a0a0a0a0a0a0au;
        n += __builtin_popcountll(~(((w & lo) + lo) | w | lo));
    }
    for (; i < s.len; i++) {
        n += s.data[i] == '\n';
    }
    return n;
}

// Code points in s, which is UTF-8: its bytes but continuation bytes.
static i64 countchars(Str s)
{
    i64 n = s.len;
    for (iz i = 0; i < s.len; i++) {
        n -= (s[i] & 0xc0) == 0x80;
    }
    return n;
}

// Whether reasoning not chosen open or closed starts folded.
static b32 thinkfolded(View *v)
{
    return v->thinking!=THINK_OPEN && !v->showthink;
}

// Whether a message takes no space and draws nothing.
static b32 msghidden(View *v, ViewMsg *m)
{
    return m->role==VIEW_THINK && v->thinking==THINK_HIDE && !v->showthink;
}

// Whether a tool row folds by its kind and outcome (tools = auto): reads,
// writes and todo lists fold; once it ends, a failure opens, the start of
// a background job folds, as the Jobs panel has it, and so does a shell
// command's output past 24 lines.
static b32 toolauto(ViewMsg *m)
{
    ViewTool *t = m->tool;
    if (t->ended) {
        if (t->status != VTOOL_OK) {
            return 0;
        } else if (t->job) {
            return 1;
        } else if (t->name == "shell") {
            return m->nl > 24;
        }
    }
    return t->name=="read" || t->name=="write" || t->name=="todo_write";
}

// Whether a tool row folds: as the user chose, else as the session's
// toggle or [ui] tools says. Arguments still arriving have no body yet,
// and a skill's instructions, pages the agent reads, stay folded but
// for a failure.
static b32 toolfolded(View *v, ViewMsg *m)
{
    ViewTool *t = m->tool;
    if (m->chosen) {
        return m->collapsed;
    } else if (t->status == VTOOL_STREAMING) {
        return 1;
    } else if (t->name=="skill" && !(t->ended && t->status!=VTOOL_OK)) {
        return 1;
    } else if (v->showtools) {
        return 0;
    }
    switch (v->toolmode) {
    case TOOLS_FOLD: return 1;
    case TOOLS_OPEN: return 0;
    }
    return toolauto(m);
}

// A copy in the text arena, which provisional messages must know about.
static Str viewclone(View *v, Str s)
{
    v->provmixed |= v->prov >= 0;
    Str r = clone(&v->text, s);
    if (!r.data) r.data = (u8 *)"";
    return r;
}

static i32 addmsg(View *v, i32 role, Str text)
{
    ViewMsg m = {};
    m.role = role;
    m.turn = v->turn;
    m.text = viewclone(v, text);
    m.nl   = countnl(text);
    m.md   = role == VIEW_ASSISTANT;
    if (role == VIEW_THINK) {
        m.chars     = countchars(text);
        m.collapsed = thinkfolded(v);
    }
    v->msgs = push(&v->meta, v->msgs, m);
    v->topsdirty  = 1;
    v->provmixed |= v->prov >= 0;
    v->stream     = -1;  // later deltas start a new message after it
    return (i32)v->msgs.len - 1;
}

// A change to the session, as a line of the transcript that the agent
// does not see: what changed (label) and to what (text).
static i32 addmark(View *v, i64 seq, Str label, Str text)
{
    i32 i = addmsg(v, VIEW_MARK, text);
    v->msgs[i].seq       = seq;
    v->msgs[i].label     = label;
    v->msgs[i].collapsed = 1;  // the header alone
    return i;
}

// Replace a message's text.
static void settext(View *v, ViewMsg *m, Str text, b32 md)
{
    m->text = viewclone(v, text);
    m->nl   = countnl(text);
    m->md   = md;
    m->ver++;
    v->topsdirty = 1;
}

// Begin a provisional message, the first of a run if none is open.
static i32 addprov(View *v, i32 role)
{
    if (v->prov < 0) {
        v->prov      = (i32)v->msgs.len;
        v->provtext  = v->text.beg;
        v->provmixed = 0;
    }
    ViewMsg m = {};
    m.role    = role;
    m.turn    = v->turn;
    m.attempt = v->attempt;
    m.prov    = 1;
    m.md      = role == VIEW_ASSISTANT;
    m.collapsed = role==VIEW_THINK && thinkfolded(v);
    v->msgs = push(&v->meta, v->msgs, m);
    v->topsdirty = 1;
    return (i32)v->msgs.len - 1;
}

// Append to the provisional message for this kind of delta, starting
// one if needed. Its text is the text arena's last allocation, so this
// extends it in place. Reasoning keeps when it arrived (ts, epoch ms).
static void viewdelta(View *v, i32 role, Str text, i64 ts)
{
    if (v->stream<0 || v->streamkind!=role) {
        v->stream     = addprov(v, role);
        v->streamkind = role;
    }
    ViewMsg *m = &v->msgs[v->stream];
    m->text = concat(&v->text, m->text, text);
    m->nl  += countnl(text);
    if (role == VIEW_THINK) {
        m->chars += countchars(text);
        m->ts0    = m->ts0 ? m->ts0 : ts;
        m->ts1    = ts;
    }
    m->ver++;
    v->deltas++;
    v->deltabytes += text.len;
    v->topsdirty   = 1;
}

// Arguments of a call streaming in: a provisional tool row per block.
static void viewargs(View *v, i32 block, Str id, Str name, Str text)
{
    ViewTool *t = 0;
    for (iz i = v->prov<0 ? v->msgs.len : v->prov; i < v->msgs.len; i++) {
        ViewMsg *m = &v->msgs[i];
        if (m->prov && m->role==VIEW_TOOL && m->tool->block==block) {
            t = m->tool;
        }
    }
    if (!t) {
        i32 i = addprov(v, VIEW_TOOL);
        t = alloc<ViewTool>(&v->aux);
        t->block  = block;
        t->status = VTOOL_STREAMING;
        v->msgs[i].tool      = t;
        v->msgs[i].collapsed = toolfolded(v, &v->msgs[i]);
        v->stream = -1;  // text after it is a new message
    }
    if (id.len)   t->call = clone(&v->aux, id);
    if (name.len) t->name = clone(&v->aux, name);
    t->args = concat(&v->aux, t->args, text);
    v->deltas++;
    v->deltabytes += text.len;
}

// Drop the provisional messages, which a committed record replaces.
// Their text is reclaimed unless other messages came in between. A
// selection's positions in messages that stay move with them. Those in
// provisional messages stay put for the commit to come, whose messages
// take their places, unless other messages came between: then nothing
// will, and the selection goes. Either way a position may now lie past
// the end, so callers clamp the selection once done (clampsel).
static void dropprov(View *v)
{
    if (v->prov >= 0) {
        Sel *s     = &v->sel;
        Pos *ps[]  = {&s->anchor, &s->anchor1, &s->focus};
        b32  mixed = 0;
        b32  inprov = 0;
        for (iz i = v->prov; i < v->msgs.len; i++) {
            mixed |= !v->msgs[i].prov;
            for (iz k = 0; k < countof(ps); k++) {
                inprov |= v->msgs[i].prov && ps[k]->msg==i;
            }
        }
        if (s->active && mixed && inprov) {
            *s = {};
        }
        iz n = v->prov;
        for (iz i = v->prov; i < v->msgs.len; i++) {
            if (v->msgs[i].prov) {
                continue;
            }
            for (iz k = 0; s->active && k < countof(ps); k++) {
                ps[k]->msg = ps[k]->msg==i ? (i32)n : ps[k]->msg;
            }
            v->msgs[n++] = v->msgs[i];
        }
        v->msgs.len = n;
        if (!v->provmixed) {
            v->text.beg = v->provtext;
        }
        v->prov      = -1;
        v->topsdirty = 1;
    }
    v->stream = -1;
}

// A failed attempt's provisional messages stay faded until the next one
// begins.
static void fadeprov(View *v)
{
    for (iz i = v->prov<0 ? v->msgs.len : v->prov; i < v->msgs.len; i++) {
        v->msgs[i].faded |= v->msgs[i].prov;
    }
    v->stream = -1;
}

// The provisional reasoning, in order, whose state the messages that
// replace it take over (takethink).
static Slice<ViewMsg> provthinks(View *v, Arena *a)
{
    Slice<ViewMsg> r = {};
    for (iz i = v->prov<0 ? v->msgs.len : v->prov; i < v->msgs.len; i++) {
        ViewMsg *m = &v->msgs[i];
        if (m->prov && !m->faded && m->role==VIEW_THINK) {
            r = push(a, r, *m);
        }
    }
    return r;
}

// The nth reasoning of a response takes over its provisional one's
// times, and whether the user opened or closed it.
static void takethink(ViewMsg *m, Slice<ViewMsg> was, iz n)
{
    if (n >= was.len) {
        return;
    }
    ViewMsg *w = &was[n];
    if (w->ts0) {
        m->ts0 = w->ts0;
        m->ts1 = w->ts1;
    }
    if (w->chosen) {
        m->chosen    = 1;
        m->collapsed = w->collapsed;
    }
}

// The provisional tool rows the user opened or closed, whose state the
// committed rows of the same calls take over.
static Slice<ViewMsg> provchosen(View *v, Arena *a)
{
    Slice<ViewMsg> r = {};
    for (iz i = v->prov<0 ? v->msgs.len : v->prov; i < v->msgs.len; i++) {
        ViewMsg *m = &v->msgs[i];
        if (m->prov && m->chosen && m->role==VIEW_TOOL && m->tool->call.len) {
            r = push(a, r, *m);
        }
    }
    return r;
}

// Fold or open all reasoning as the setting and the session's toggle
// say, but what the user opened or closed, and hide it or show it.
static void rethink(View *v)
{
    for (iz i = 0; i < v->msgs.len; i++) {
        ViewMsg *m = &v->msgs[i];
        if (m->role == VIEW_THINK) {
            m->collapsed = m->chosen ? m->collapsed : thinkfolded(v);
            m->measured  = 0;
            m->ver++;
        }
    }
    v->topsdirty = 1;
}

// Show reasoning as [ui] thinking says (THINK_*), all of it at once but
// what the user opened or closed. Returns whether anything changed.
static b32 viewthink(View *v, i32 mode)
{
    if (v->thinking == mode) {
        return 0;
    }
    v->thinking = mode;
    rethink(v);
    return 1;
}

// The session's toggle: all reasoning open, hidden or not, or else as
// the setting says. Returns whether anything changed.
static b32 viewshowthink(View *v, b32 on)
{
    if (v->showthink == on) {
        return 0;
    }
    v->showthink = on;
    rethink(v);
    return 1;
}

// Fold or open every tool row the user did not choose, as the setting
// and the session's toggle now say.
static void retool(View *v)
{
    for (iz i = 0; i < v->msgs.len; i++) {
        ViewMsg *m = &v->msgs[i];
        if (m->role != VIEW_TOOL) {
            continue;
        }
        b32 c = toolfolded(v, m);
        if (c != m->collapsed) {
            m->collapsed = c;
            m->measured  = 0;
            m->ver++;
            v->topsdirty = 1;
        }
    }
}

// Fold tool rows as [ui] tools says (TOOLS_*), all at once but what the
// user opened or closed. Returns whether anything changed.
static b32 viewtools(View *v, i32 mode)
{
    if (v->toolmode == mode) {
        return 0;
    }
    v->toolmode = mode;
    retool(v);
    return 1;
}

// The session's toggle: every tool row open, or else as the setting
// says. Returns whether anything changed.
static b32 viewshowtools(View *v, b32 on)
{
    if (v->showtools == on) {
        return 0;
    }
    v->showtools = on;
    retool(v);
    return 1;
}

// Keep a selection inside the messages that exist.
static void clampsel(View *v)
{
    Sel *s = &v->sel;
    if (!s->active) {
        return;
    }
    i32 n = (i32)v->msgs.len;
    Pos *ps[] = {&s->anchor, &s->anchor1, &s->focus};
    for (iz i = 0; i < countof(ps); i++) {
        Pos *p = ps[i];
        if (p->msg >= n) {
            if (!n) {
                s->active = 0;
                return;
            }
            p->msg = n - 1;
            p->off = (i32)v->msgs[n-1].text.len;
        }
        if (p->off > v->msgs[p->msg].text.len) {
            p->off = (i32)v->msgs[p->msg].text.len;
        }
    }
}


// Tool rows

static i32 vtoolstatus(Str s)
{
    for (i32 i = 0; i < VTOOL_COUNT; i++) {
        if (vtoolnames[i] == s) {
            return i;
        }
    }
    return VTOOL_UNKNOWN;
}

// One line of at most VIEW_SUMMARY bytes, cut on a UTF-8 boundary, with
// an ellipsis when cut or when more lines follow.
static Str oneline(Arena *a, Str s)
{
    s = trim(s);
    Cut c    = cut(s, '\n');
    Str r    = trimright(c.head);
    b32 more = c.ok;
    if (r.len > VIEW_SUMMARY) {
        iz n = VIEW_SUMMARY;
        for (; n>0 && (r[n]&0xc0)==0x80; n--) {}
        r    = takehead(r, n);
        more = 1;
    }
    return more ? concat(a, clone(a, r), "\xe2\x80\xa6") : clone(a, r);
}

// What a call is about, from its arguments: the command or the path, a
// path inside the working directory relative to it.
// The memories of an index: each line linking NAME.md, its words after
// the link. Lines the user wrote otherwise are not memories.
static Slice<ViewMem> viewmems(Arena *a, Str index)
{
    Slice<ViewMem> r = {};
    for (Str rest = index; rest.len;) {
        Cut c = cut(rest, '\n');
        rest = c.tail;
        Str line = trim(c.head);
        iz  beg  = find(line, "[");
        iz  mid  = find(line, "](");
        if (beg<0 || mid<beg) {
            continue;
        }
        Str link = cuthead(line, mid+2);
        iz  end  = find(link, ".md)");
        if (end < 0) {
            continue;
        }
        ViewMem m = {};
        m.name = clone(a, takehead(link, end));
        Str desc = trim(cuthead(link, end+4));
        if (startswith(desc, "\xe2\x80\x94") || startswith(desc, "-") || startswith(desc, ":")) {
            desc = trim(cuthead(desc, startswith(desc, "\xe2") ? 3 : 1));
        }
        m.desc = clone(a, desc);
        r = push(a, r, m);
    }
    return r;
}

static Str toolsummary(Arena *perm, Arena scratch, Str name, Str args, Str cwd)
{
    Json *j     = jsonparse(&scratch, args).root;
    Str   cmd   = jsonstr(jsonget(j, "command"));
    Str   p     = jsonstr(jsonget(j, "path"));
    Str   desc  = jsonstr(jsonget(j, "description"));
    Json *patch = jsonget(j, "patch");
    Str   shell = jsonstr(jsonget(j, "shell"));
    if (name=="shell" && cmd.data && shell.len) {
        // A call that named a shell of [tools] shells says which
        return concat(perm, concat(&scratch, "in ", shell), concat(&scratch, ": ", oneline(&scratch, cmd)));
    } else if (name=="shell" && cmd.data) {
        return oneline(perm, cmd);
    } else if (name == "todo_write") {
        // The list may come as a string holding JSON
        Json *t = jsonget(j, "todos");
        if (t->type == JSON_STRING) {
            t = jsonparse(&scratch, t->str).root;
        }
        i64 n = 0, done = 0, active = 0;
        for (Json *e = t->child; e; e = e->next, n++) {
            Str st = jsonstr(jsonget(e, "status"));
            done   += st == "done";
            active += st == "in_progress";
        }
        Buf b(perm, 64);
        print(&b, n);
        print(&b, n==1 ? Str(" item, ") : Str(" items, "));
        print(&b, done);
        print(&b, " done");
        if (active) {
            print(&b, ", ");
            print(&b, active);
            print(&b, " in progress");
        }
        return finish(&b);
    } else if (name=="task" && desc.len) {
        return oneline(perm, desc);
    } else if (name == "skill") {
        Str skill = jsonstr(jsonget(j, "name"));
        return oneline(perm, skill.len ? skill : args);
    } else if (name == "send_message") {
        // The target as given, until the result names its title
        Str to = jsonstr(jsonget(j, "to"));
        return oneline(perm, concat(&scratch, "Message \xe2\x86\x92 ", to));
    } else if (name == "memory") {
        Str act = jsonstr(jsonget(j, "action"));
        Str mem = jsonstr(jsonget(j, "name"));
        return oneline(perm, concat(&scratch, act=="forget" ? Str("Forget ") : Str("Save "), mem));
    } else if (name=="edit" && patch->type==JSON_STRING) {
        // The files the patch names, as it names them
        Slice<Str> paths = patchpaths(&scratch, patch->str);
        Buf        b(&scratch, 256);
        for (iz i = 0; i < paths.len; i++) {
            print(&b, i ? Str(", ") : Str(""));
            print(&b, paths[i]);
        }
        return oneline(perm, paths.len ? finish(&b) : Str("patch"));
    } else if (p.data) {
        if (cwd.len && p.len>cwd.len+1 && startswith(p, cwd) &&
                (p[cwd.len]=='/' || p[cwd.len]=='\\')) {
            p = cuthead(p, cwd.len+1);
        }
        return oneline(perm, p);
    }
    return oneline(perm, args);
}

// The longest run of backticks in s, so a fence can be longer.
static iz backticks(Str s)
{
    iz best = 0, run = 0;
    for (iz i = 0; i < s.len; i++) {
        run  = s[i]=='`' ? run+1 : 0;
        best = run>best ? run : best;
    }
    return best;
}

static void difflines(Buf *b, u8 sign, Str s)
{
    if (!s.len) {
        return;
    }
    if (s[s.len-1] == '\n') {
        s = cuttail(s, 1);
    }
    for (Str rest = s;;) {
        Cut c = cut(rest, '\n');
        putbyte(b, sign);
        print(b, c.head);
        putbyte(b, '\n');
        if (!c.ok) break;
        rest = c.tail;
    }
}

static i64 linecount(Str s)
{
    if (!s.len) return 0;
    return countnl(s) + (s[s.len-1] != '\n');
}

// A finished edit or write as a fenced unified diff (markdown), from its
// arguments and the line its result names; null if not one. Its
// temporaries are left in the arena too.
static Str tooldiff(Arena *a, Str name, Str args, Str result)
{
    Json *j     = jsonparse(a, args).root;
    Str   path  = jsonstr(jsonget(j, "path"));
    Json *patch = jsonget(j, "patch");
    Json *edits = jsonget(j, "edits");
    Buf   d(a, 1<<12);
    if (name=="edit" && patch->type==JSON_STRING) {
        // An apply_patch envelope (edit_format patch): its file headers
        // as a unified diff's, its hunks as they are
        for (Str rest = patch->str; rest.len;) {
            Cut c  = cut(rest, '\n');
            Str l  = c.head.len && c.head[c.head.len-1]=='\r' ? cuttail(c.head, 1) : c.head;
            Str hd = trim(l);
            rest = c.tail;
            if (hd=="*** Begin Patch" || hd=="*** End Patch" || hd=="*** End of File") {
                continue;
            } else if (startswith(hd, "*** Update File:")) {
                Str p = trim(cuthead(hd, 16));
                print(&d, "--- ");
                print(&d, p);
                print(&d, "\n+++ ");
                print(&d, p);
            } else if (startswith(hd, "*** Add File:")) {
                print(&d, "--- /dev/null\n+++ ");
                print(&d, trim(cuthead(hd, 13)));
            } else if (startswith(hd, "*** Delete File:")) {
                print(&d, "--- ");
                print(&d, trim(cuthead(hd, 16)));
                print(&d, "\n+++ /dev/null");
            } else if (startswith(hd, "*** Move to:")) {
                print(&d, "rename to ");
                print(&d, trim(cuthead(hd, 12)));
            } else {
                print(&d, l);
            }
            putbyte(&d, '\n');
        }
    } else if (name=="edit" && edits->type==JSON_ARRAY) {
        // Line edits (edit_format hashline): what each puts where
        print(&d, "--- ");
        print(&d, path);
        print(&d, "\n+++ ");
        print(&d, path);
        putbyte(&d, '\n');
        for (Json *e = edits->child; e; e = e->next) {
            Str op   = jsonstr(jsonget(e, "op"));
            i64 line = jsonint(jsonget(e, "line"), 0);
            i64 end  = jsonint(jsonget(e, "end"), line);
            print(&d, "@@ ");
            print(&d, op);
            print(&d, op=="insert" ? Str(" after line ") : Str(" lines "));
            print(&d, line);
            if (op!="insert" && end!=line) {
                putbyte(&d, '-');
                print(&d, end);
            }
            print(&d, " @@\n");
            difflines(&d, '+', jsonstr(jsonget(e, "text")));
        }
    } else if (name == "edit") {
        Str olds = jsonstr(jsonget(j, "old_string"));
        Str news = jsonstr(jsonget(j, "new_string"));
        iz  at   = find(result, "line ");
        Int line = at<0 ? Int{} : parseint(cuthead(result, at+5));
        if (!line.ok) {  // "line 12." or "line 12 (...)": the digits only
            iz e = at + 5;
            for (; e<result.len && result[e]>='0' && result[e]<='9'; e++) {}
            line = at<0 ? Int{} : parseint(span(result.data+at+5, result.data+e));
        }
        i64 n = line.ok ? line.val : 1;
        print(&d, "--- ");
        print(&d, path);
        print(&d, "\n+++ ");
        print(&d, path);
        print(&d, "\n@@ -");
        print(&d, n);
        putbyte(&d, ',');
        print(&d, linecount(olds));
        print(&d, " +");
        print(&d, n);
        putbyte(&d, ',');
        print(&d, linecount(news));
        print(&d, " @@\n");
        difflines(&d, '-', olds);
        difflines(&d, '+', news);
    } else if (name == "write") {
        Str content = jsonstr(jsonget(j, "content"));
        i64 lines   = linecount(content);
        b32 more    = content.len > VIEW_DIFFMAX;
        if (more) {
            iz n = VIEW_DIFFMAX;
            for (; n>0 && content[n-1]!='\n'; n--) {}
            content = takehead(content, n);
        }
        print(&d, "--- /dev/null\n+++ ");
        print(&d, path);
        print(&d, "\n@@ -0,0 +1,");
        print(&d, lines);
        print(&d, " @@\n");
        difflines(&d, '+', content);
        if (more) {
            print(&d, "\\ ");
            print(&d, lines - linecount(content));
            print(&d, " more lines\n");
        }
    } else {
        return {};
    }
    Str diff  = finish(&d);
    iz  ticks = backticks(diff) + 1;
    ticks = ticks<3 ? 3 : ticks;
    Buf b(a, diff.len + 2*ticks + 8);
    for (iz i = 0; i < ticks; i++) putbyte(&b, '`');
    print(&b, "diff\n");
    print(&b, diff);
    for (iz i = 0; i < ticks; i++) putbyte(&b, '`');
    putbyte(&b, '\n');
    return finish(&b);
}

// The row of a call, most recent first; -1 if none.
static i32 findcall(View *v, Str call)
{
    for (iz i = v->msgs.len-1; i >= 0; i--) {
        ViewMsg *m = &v->msgs[i];
        if (m->role==VIEW_TOOL && !m->prov && m->tool->call==call) {
            return (i32)i;
        }
    }
    return -1;
}

static i32 findask(View *v, Str req)
{
    for (iz i = v->msgs.len-1; i >= 0; i--) {
        ViewMsg *m = &v->msgs[i];
        if (m->role==VIEW_ASK && m->tool->req==req) {
            return (i32)i;
        }
    }
    return -1;
}

static ViewJob *findjob(View *v, i64 n)
{
    for (iz i = v->jobs.len-1; i >= 0; i--) {
        if (v->jobs[i].n == n) {
            return &v->jobs[i];
        }
    }
    return 0;
}

static i32 vjobstatus(Str s)
{
    for (i32 i = 0; i < VJOB_COUNT; i++) {
        if (vjobnames[i] == s) {
            return i;
        }
    }
    return VJOB_UNKNOWN;
}

// A path as the user thinks of it: relative to the working directory
// when inside it.
static Str showpath(View *v, Str p)
{
    Str cwd = v->cwd;
    if (cwd.len && p.len>cwd.len+1 && startswith(p, cwd) &&
            (p[cwd.len]=='/' || p[cwd.len]=='\\')) {
        return cuthead(p, cwd.len+1);
    }
    return p;
}

// A markdown code span holding s, whatever backticks it has.
static void codespan(Buf *b, Str s)
{
    iz ticks = backticks(s) + 1;
    b32 pad  = ticks > 1;
    for (iz i = 0; i < ticks; i++) putbyte(b, '`');
    if (pad) putbyte(b, ' ');
    for (iz i = 0; i < s.len; i++) {
        putbyte(b, s[i]=='\n' ? ' ' : s[i]);
    }
    if (pad) putbyte(b, ' ');
    for (iz i = 0; i < ticks; i++) putbyte(b, '`');
}

// The snapshots a rewind would put back, newest first, one per file:
// those after a call's start (to a call), or from a turn on (else),
// that no rewind undid yet. Each path's oldest snapshot in range says
// whether the file is restored or removed.
static Slice<ViewSnap> rewindset(View *v, Arena *a, Str call, i32 turn)
{
    i64 seq0 = -1;
    if (call.len) {
        i32 i = findcall(v, call);
        if (i < 0) {
            return {};
        }
        seq0 = v->msgs[i].tool->startseq;
    }
    Slice<ViewSnap> r = {};
    for (iz i = v->snaps.len-1; i >= 0; i--) {
        ViewSnap *s = &v->snaps[i];
        if (s->undone || (call.len ? s->seq<=seq0 : s->turn<turn)) {
            continue;
        }
        b32 dup = 0;
        for (iz k = 0; k < r.len; k++) {
            if (r[k].path == s->path) {
                r[k] = *s;  // an older change of the same file decides
                dup  = 1;
            }
        }
        if (!dup) {
            r = push(a, r, *s);
        }
    }
    return r;
}

// The turn a fork at a message is cut after (fork's turn), or -1 if
// none: the turn before the one a prompt began (before), else the
// message's own. Both are the engine's numbers, from its turn records'
// n, so that turns it began by itself (via job or compact) count too.
static i64 viewforkat(ViewMsg *m, b32 *before)
{
    *before = m->role==VIEW_USER && m->startsturn>0;
    if (*before) {
        return m->startsturn - 1;
    }
    return m->turn>0 ? m->turn : -1;
}

// Whether a rewind from a turn on would change any file.
static b32 turnrewinds(View *v, i32 turn)
{
    for (iz i = v->snaps.len-1; i >= 0; i--) {
        if (!v->snaps[i].undone && v->snaps[i].turn>=turn) {
            return 1;
        }
    }
    return 0;
}

// The snapshot of the file a tool row changed, or null.
static ViewSnap *rowsnap(View *v, ViewTool *t)
{
    if (!t || t->snap<=0 || t->snap>v->snaps.len) {
        return 0;
    }
    return &v->snaps[t->snap-1];
}

// A committed call's row.
static i32 addcall(View *v, Arena scratch, Str id, Str name, Str args)
{
    i32       i = addmsg(v, VIEW_TOOL, "");
    ViewTool *t = alloc<ViewTool>(&v->aux);
    t->call    = clone(&v->aux, id);
    t->name    = clone(&v->aux, name);
    t->args    = clone(&v->aux, args);
    t->summary = toolsummary(&v->aux, scratch, name, args, v->cwd);
    t->status  = VTOOL_CALLED;
    v->msgs[i].tool      = t;
    v->msgs[i].collapsed = toolfolded(v, &v->msgs[i]);
    return i;
}

// The host a task call's subagent runs on, once known.
static void taskhost(View *v, ViewTool *t, Str host)
{
    if (host.len && !t->host.len) {
        t->host = clone(&v->aux, host);
        v->hostcalls++;
    }
}

// The body of a finished call: a diff for edits and writes that worked,
// otherwise the result the agent read. Where the whole output is, which
// the agent's text names in a last line, the row's Output action has.
static void toolbody(View *v, Arena scratch, ViewMsg *m, Str text)
{
    ViewTool *t = m->tool;
    if (t->status == VTOOL_OK) {
        Str diff = tooldiff(&scratch, t->name, t->args, text);
        if (diff.data) {
            settext(v, m, diff, 1);
            return;
        }
    }
    if (t->name=="task" && t->status==VTOOL_OK && !t->spill.len) {
        settext(v, m, text, 1);
        return;
    }
    settext(v, m, text, 0);
}


// Records

// The context gauge's numbers a record has. Returns whether any changed.
static b32 viewgauge(View *v, Json *j)
{
    Json *ctx = jsonget(j, "ctx");
    Json *at  = jsonget(j, "compact_at");
    Json *tok = jsonget(j, "tokens");
    i64   was[] = {v->ctx, v->compactat, v->tokens};
    v->ctx       = ctx->type==JSON_NUMBER ? jsonint(ctx, 0) : v->ctx;
    v->compactat = at->type==JSON_NUMBER  ? jsonint(at, 0)  : v->compactat;
    v->tokens    = tok->type==JSON_NUMBER ? jsonint(tok, 0) : v->tokens;
    return was[0]!=v->ctx || was[1]!=v->compactat || was[2]!=v->tokens;
}

static i32 livestate(Str s)
{
    for (i32 i = 0; i < LIVE_COUNT; i++) {
        if (livenames[i] == s) {
            return i;
        }
    }
    return LIVE_IDLE;
}

// Apply one parsed record. Returns 1 if what the view shows changed.
static b32 viewrecord(View *v, Arena scratch, EvRec *r)
{
    if (!r->ok) {
        return 0;
    }
    if (r->seq) {
        if (r->seq <= v->lastseq) {
            return 0;  // applied already, before a resync
        } else if (r->seq!=v->lastseq+1 && !v->loose) {
            v->lost = 1;  // records were lost: the resync brings them
            return 0;
        }
        v->lastseq = r->seq;
    }

    Json *j = r->root;
    switch (r->ev) {
    case EV_SESSION:
        v->cwd      = clone(&v->aux, jsonstr(jsonget(j, "cwd")));
        v->nofolder = jsonint(jsonget(j, "nofolder"), 0) == 1;
        v->model    = clone(&v->aux, jsonstr(jsonget(j, "model")));
        v->endpoint = clone(&v->aux, jsonstr(jsonget(j, "endpoint")));
        v->role     = clone(&v->aux, jsonstr(jsonget(j, "role")));
        v->parent   = clone(&v->aux, jsonstr(jsonget(j, "parent")));
        v->pcall    = clone(&v->aux, jsonstr(jsonget(j, "call")));
        v->remote   = jsonint(jsonget(j, "remote"), 0) == 1;
        v->depth    = (i32)jsonint(jsonget(j, "depth"), 0);
        v->roleprompt = clone(&v->aux, jsonstr(jsonget(j, "prompt")));
        v->scratch    = clone(&v->aux, jsonstr(jsonget(j, "scratch")));
        v->forkedfrom = clone(&v->aux, jsonstr(jsonget(j, "forked_from")));
        v->forkturn   = jsonint(jsonget(j, "fork_turn"), 0);
        v->sideof     = clone(&v->aux, jsonstr(jsonget(j, "side_of")));
        {
            Buf b(&scratch, 256);
            for (Json *t = jsonget(j, "tools")->child; t; t = t->next) {
                if (b.len) putbyte(&b, ' ');
                print(&b, jsonstr(t));
            }
            v->tools = clone(&v->aux, finish(&b));
        }
        if (v->state == LIVE_CLOSED) {
            v->state = LIVE_IDLE;
        }
        return 1;

    case EV_TITLE:
        v->title = clone(&v->aux, jsonstr(jsonget(j, "text")));
        if (jsonstr(jsonget(j, "by")) == "user") {
            addmark(v, r->seq, "Renamed", v->title);
        }
        return 1;

    case EV_MODEL: {
        // Switched for the rest of the session (model)
        v->model    = clone(&v->aux, jsonstr(jsonget(j, "model")));
        v->endpoint = clone(&v->aux, jsonstr(jsonget(j, "endpoint")));
        i32 i = addmark(v, r->seq, "Model", v->model);
        v->msgs[i].from = v->endpoint;
    } return 1;

    case EV_PREFIX:
        // The shell it began in, until a shell record: a prefix record
        // written again later (MCP tools) names that one still
        if (!v->shellset) {
            Str path = jsonstr(jsonget(j, "shell"));
            Str name = jsonstr(jsonget(j, "shell_name"));
            v->shellpath = clone(&v->aux, path);
            v->shell     = clone(&v->aux, name.len ? name : basename(path));
        }
        return 1;

    case EV_SHELL: {
        // Switched for the rest of the session (shell)
        v->shell     = clone(&v->aux, jsonstr(jsonget(j, "name")));
        v->shellpath = clone(&v->aux, jsonstr(jsonget(j, "path")));
        v->shellset  = 1;
        i32 i = addmark(v, r->seq, "Shell", v->shell);
        v->msgs[i].from = v->shellpath;
    } return 1;

    case EV_MODE: {
        Str path   = jsonstr(jsonget(j, "path"));
        b32 plan   = jsonstr(jsonget(j, "mode")) == "plan";
        b32 bypass = jsonstr(jsonget(j, "mode")) == "bypass";
        if (plan!=v->plan || bypass!=v->bypass) {
            addmark(v, r->seq, plan   ? Str("Plan mode")   :
                               bypass ? Str("Bypass mode") : Str("Build mode"),
                    plan   ? Str("the agent changes nothing but its plan") :
                    bypass ? Str("the agent asks nothing, inside or outside its directory") :
                             Str("plan mode and Bypass are off"));
        }
        v->plan     = plan;
        v->bypass   = bypass;
        v->planpath = path==v->planpath ? v->planpath : clone(&v->aux, path);
        v->planver++;
    } return 1;

    case EV_USER: {
        // A skill the user invoked shows as typed, /name and what
        // followed it, and the instructions it sent fold below
        dropprov(v);
        Str via   = jsonstr(jsonget(j, "via"));
        Str text  = jsonstr(jsonget(j, "text"));
        Str skill = jsonstr(jsonget(j, "skill"));
        Str args  = jsonstr(jsonget(j, "args"));
        Str typed = text;
        if (skill.len) {
            Buf b(&scratch, 64 + args.len);
            putbyte(&b, '/');
            print(&b, skill);
            print(&b, args.len ? Str(" ") : Str(""));
            print(&b, args);
            typed = finish(&b);
        }
        i32 i = addmsg(v, VIEW_USER, typed);
        v->msgs[i].seq   = r->seq;
        for (Json *k = jsonget(j, "images")->child; k; k = k->next) {
            ViewImage im = {};
            im.name = clone(&v->aux, jsonstr(jsonget(k, "name")));
            im.mime = clone(&v->aux, jsonstr(jsonget(k, "mime")));
            im.w    = (i32)jsonint(jsonget(k, "w"), 0);
            im.h    = (i32)jsonint(jsonget(k, "h"), 0);
            v->msgs[i].images = push(&v->aux, v->msgs[i].images, im);
        }
        v->msgs[i].label = via=="steer" ? Str("You (steering)") :
                           via=="queue" ? Str("You (queued)") :
                           via=="task"  ? Str("Task from the parent agent") : Str{};
        if (skill.len) {
            iz  end  = find(text, "\n</skill>");
            Str body = end<0 ? text : takehead(text, end + 9);
            i32 k    = addmsg(v, VIEW_NOTE, body);
            v->msgs[k].seq       = r->seq;
            v->msgs[k].fold      = 1;
            v->msgs[k].collapsed = 1;
            v->msgs[k].label     = concat(&v->aux, "To the agent: skill ", clone(&v->aux, skill));
        }
        clampsel(v);
    } return 1;

    case EV_TURN: {
        v->turn    = (i32)jsonint(jsonget(j, "n"), v->turn+1);
        v->attempt = 0;
        v->busy    = 1;
        v->turnts  = r->ts;
        v->pdone   = v->ptotal = 0;
        v->turncompact = jsonstr(jsonget(j, "via")) == "compact";
        // The message that began it: a prompt just before, or, for a
        // turn the session began by itself, a note saying why
        Str via = jsonstr(jsonget(j, "via"));
        if (via == "job") {
            i32 i = addmsg(v, VIEW_NOTE, "A background job ended, so the "
                           "agent went on without a prompt.");
            v->msgs[i].seq        = r->seq;
            v->msgs[i].label      = "Resumed by itself";
            v->msgs[i].startsturn = v->turn;
        } else if (via == "task") {
            i32 i = addmsg(v, VIEW_NOTE, "A subagent moved to the background "
                           "finished, so the agent went on without a prompt.");
            v->msgs[i].seq        = r->seq;
            v->msgs[i].label      = "Resumed by itself";
            v->msgs[i].startsturn = v->turn;
        } else if (via == "compact") {
            // Asked for by the user (compact), with no message of its
            // own: its card (EV_COMPACT) says what it did
        } else if (v->msgs.len) {
            ViewMsg *m = &v->msgs[v->msgs.len-1];
            if (m->role==VIEW_USER && !m->startsturn && m->label!="You (steering)") {
                m->startsturn = v->turn;
            }
        }
    } return 1;

    case EV_ATTEMPT:
        dropprov(v);  // a failed attempt's, shown faded until now
        clampsel(v);
        v->attempt = (i32)jsonint(jsonget(j, "n"), v->attempt+1);
        v->pdone   = v->ptotal = 0;
        viewgauge(v, j);
        return 1;

    case EV_DELTA: {
        i32 turn    = (i32)jsonint(jsonget(j, "turn"), 0);
        i32 attempt = (i32)jsonint(jsonget(j, "attempt"), 0);
        if (turn<v->doneturn || (turn==v->doneturn && attempt<=v->doneattempt)) {
            return 0;  // committed already
        }
        v->lastdata = r->ts;
        Str kind = jsonstr(jsonget(j, "kind"));
        Str text = jsonstr(jsonget(j, "text"));
        if (kind == "args") {
            viewargs(v, (i32)jsonint(jsonget(j, "block"), 0),
                     jsonstr(jsonget(j, "id")), jsonstr(jsonget(j, "name")), text);
        } else {
            viewdelta(v, kind=="think" ? VIEW_THINK : VIEW_ASSISTANT, text, r->ts);
        }
    } return 1;

    case EV_SNAPSHOT: {
        // What streamed before this view listened (an open mid-stream):
        // it replaces whatever is provisional, and later deltas extend it
        i32 turn    = (i32)jsonint(jsonget(j, "turn"), 0);
        i32 attempt = (i32)jsonint(jsonget(j, "attempt"), 0);
        if (turn<v->doneturn || (turn==v->doneturn && attempt<=v->doneattempt)) {
            return 0;  // committed already
        }
        Slice<ViewMsg> was = provthinks(v, &scratch);
        dropprov(v);
        v->lastdata = r->ts;
        for (Json *b = jsonget(j, "blocks")->child; b; b = b->next) {
            Str kind = jsonstr(jsonget(b, "kind"));
            Str text = jsonstr(jsonget(b, "text"));
            if (jsonint(jsonget(b, "cut"), 0)) {
                text = concat(&scratch, "\xe2\x80\xa6", text);  // ellipsis
            }
            if (kind == "args") {
                viewargs(v, (i32)jsonint(jsonget(b, "block"), 0),
                         jsonstr(jsonget(b, "id")), jsonstr(jsonget(b, "name")), text);
            } else {
                viewdelta(v, kind=="think" ? VIEW_THINK : VIEW_ASSISTANT, text, r->ts);
            }
        }
        iz n = 0;
        for (iz i = v->prov<0 ? v->msgs.len : v->prov; i < v->msgs.len; i++) {
            if (v->msgs[i].prov && v->msgs[i].role==VIEW_THINK) {
                takethink(&v->msgs[i], was, n++);
            }
        }
        clampsel(v);
    } return 1;

    case EV_ASSISTANT: {
        Slice<ViewMsg> was    = provthinks(v, &scratch);
        Slice<ViewMsg> chosen = provchosen(v, &scratch);
        iz             n      = 0;  // reasoning so far
        dropprov(v);
        // Its usage: what the context holds after it, the prompt and the
        // reply, and how much of the prompt the server had cached
        Json *u = jsonget(j, "usage");
        if (u->type == JSON_OBJECT) {
            i64 in  = jsonint(jsonget(u, "in"), 0);
            i64 out = jsonint(jsonget(u, "out"), 0);
            v->tokens = in>0 ? in+out : v->tokens;
            v->cached = jsonint(jsonget(u, "cached"), 0);
        }
        v->doneturn    = (i32)jsonint(jsonget(j, "turn"), v->turn);
        v->doneattempt = (i32)jsonint(jsonget(j, "attempt"), v->attempt);
        Json *blocks = jsonget(j, "blocks");
        for (Json *b = blocks->child; b; b = b->next) {
            Str t = jsonstr(jsonget(b, "t"));
            i32 i = -1;
            if (t=="think" && !jsonstr(jsonget(b, "text")).len) {
                continue;  // reasoning a server kept to itself (Anthropic)
            } else if (t == "think") {
                i = addmsg(v, VIEW_THINK, jsonstr(jsonget(b, "text")));
                takethink(&v->msgs[i], was, n++);
            } else if (t == "text") {
                i = addmsg(v, VIEW_ASSISTANT, jsonstr(jsonget(b, "text")));
            } else if (t == "call") {
                Str id = jsonstr(jsonget(b, "id"));
                i = addcall(v, scratch, id, jsonstr(jsonget(b, "name")),
                            jsonstr(jsonget(b, "args")));
                for (iz k = 0; k < chosen.len; k++) {
                    if (chosen[k].tool->call == id) {
                        v->msgs[i].chosen    = 1;
                        v->msgs[i].collapsed = chosen[k].collapsed;
                    }
                }
            } else {
                continue;
            }
            v->msgs[i].seq     = r->seq;
            v->msgs[i].attempt = v->doneattempt;
        }
        clampsel(v);
    } return 1;

    case EV_TOOLSTART: {
        Str call = jsonstr(jsonget(j, "call"));
        i32 i    = findcall(v, call);
        if (i < 0) {
            i = addcall(v, scratch, call, jsonstr(jsonget(j, "name")),
                        jsonstr(jsonget(j, "args")));
            v->msgs[i].seq = r->seq;
        }
        ViewMsg *m = &v->msgs[i];
        m->tool->status   = VTOOL_RUNNING;
        m->tool->start    = r->ts;
        m->tool->startseq = r->seq;
        if (m->tool->name == "task") {
            Json *args = jsonparse(&scratch, m->tool->args).root;
            taskhost(v, m->tool, jsonstr(jsonget(args, "host")));
        }
        m->ver++;
    } return 1;

    case EV_TASKHOST: {
        // A task's subagent named by the host it runs on
        i32 i = findcall(v, jsonstr(jsonget(j, "call")));
        if (i < 0) {
            return 0;
        }
        ViewMsg  *m   = &v->msgs[i];
        ViewTool *t   = m->tool;
        Str       kid = jsonstr(jsonget(j, "child"));
        taskhost(v, t, jsonstr(jsonget(j, "host")));
        if (kid.len) {
            t->child = clone(&v->aux, kid);
        }
        m->ver++;
    } return 1;

    case EV_TOOLOUT: {
        i32 i = findcall(v, jsonstr(jsonget(j, "call")));
        if (i<0 || v->msgs[i].tool->status!=VTOOL_RUNNING) {
            return 0;  // ended already
        }
        ViewMsg  *m    = &v->msgs[i];
        ViewTool *t    = m->tool;
        Str       tail = jsonstr(jsonget(j, "tail"));
        if (tail.len > t->tailcap) {
            t->tailcap = tail.len>4096 ? tail.len : 4096;
            t->tail    = allocbytes(&v->aux, t->tailcap);
        }
        copybytes(t->tail, tail.data, tail.len);
        t->outbytes = jsonint(jsonget(j, "bytes"), t->outbytes);
        m->text = {t->tail, tail.len};
        m->nl   = countnl(m->text);
        m->ver++;
        v->outts     = r->ts;
        v->topsdirty = 1;
    } return 1;

    case EV_TOOLEND: {
        Str call = jsonstr(jsonget(j, "call"));
        i32 i    = findcall(v, call);
        if (i < 0) {
            i = addcall(v, scratch, call, "", "");
        }
        ViewMsg  *m  = &v->msgs[i];
        ViewTool *t  = m->tool;
        Json     *ex = jsonget(j, "exit");
        t->status  = vtoolstatus(jsonstr(jsonget(j, "status")));
        t->hasexit = ex->type == JSON_NUMBER;
        t->exit    = (i32)jsonint(ex, 0);
        t->ms      = jsonint(jsonget(j, "ms"), 0);
        t->spill   = clone(&v->aux, jsonstr(jsonget(j, "spill")));
        t->bytes   = jsonint(jsonget(j, "bytes"), 0);
        t->job     = jsonint(jsonget(j, "job"), t->job);
        Str kid    = jsonstr(jsonget(j, "child"));
        t->child   = kid.len ? clone(&v->aux, kid) : t->child;  // task.host's
        taskhost(v, t, jsonstr(jsonget(j, "host")));
        Str to     = jsonstr(jsonget(j, "to"));
        Str title  = jsonstr(jsonget(j, "to_title"));
        if (t->name=="send_message" && (to.len || title.len)) {
            Str name   = title.len ? title : to;
            t->summary = oneline(&v->aux, concat(&scratch, "Message \xe2\x86\x92 ", name));
            t->child   = clone(&v->aux, to);
        }
        t->image   = clone(&v->aux, jsonstr(jsonget(j, "image")));
        t->imgw    = (i32)jsonint(jsonget(j, "width"), 0);
        t->imgh    = (i32)jsonint(jsonget(j, "height"), 0);
        m->seq     = r->seq;
        toolbody(v, scratch, m, jsonstr(jsonget(j, "text")));
        if (v->planpath.len && (t->name=="write" || t->name=="edit") &&
                find(t->args, "plan.md")>=0) {
            v->planver++;  // perhaps the plan file
        }
        // A call ends its ask, which after a crash has no answer
        for (iz i = v->msgs.len-1; i >= 0; i--) {
            ViewMsg *k = &v->msgs[i];
            if (k->role==VIEW_ASK && k->tool->call==call && k->tool->answer==VASK_PENDING) {
                k->tool->answer = VASK_DENIED;
                k->tool->by     = "interrupted";
                k->ver++;
                v->asks--;
                v->topsdirty = 1;
            }
        }
        t->ended     = 1;
        m->collapsed = toolfolded(v, m);  // unless the user chose
    } return 1;

    case EV_ASK: {
        dropprov(v);
        Str detail = jsonstr(jsonget(j, "detail"));
        Str hint   = jsonstr(jsonget(j, "hint"));
        Buf b(&scratch, detail.len + hint.len + 64);
        print(&b, detail);
        if (hint.len) {
            print(&b, "\nNearest path inside the workspace: ");
            print(&b, hint);
        }
        i32       i = addmsg(v, VIEW_ASK, finish(&b));
        ViewTool *t = alloc<ViewTool>(&v->aux);
        t->req      = clone(&v->aux, jsonstr(jsonget(j, "req")));
        t->kind     = clone(&v->aux, jsonstr(jsonget(j, "kind")));
        t->call     = clone(&v->aux, jsonstr(jsonget(j, "call")));
        t->tool     = clone(&v->aux, jsonstr(jsonget(j, "tool")));
        t->path     = clone(&v->aux, jsonstr(jsonget(j, "path")));
        t->detail   = clone(&v->aux, detail);
        t->hint     = clone(&v->aux, hint);
        t->deadline = jsonint(jsonget(j, "deadline"), r->ts);
        t->grant    = clone(&v->aux, jsonstr(jsonget(j, "always")));
        t->access   = clone(&v->aux, jsonstr(jsonget(j, "access")));
        t->scope    = clone(&v->aux, jsonstr(jsonget(j, "scope")));
        t->trust    = clone(&v->aux, jsonstr(jsonget(j, "trust")));
        v->msgs[i].tool = t;
        v->msgs[i].seq  = r->seq;
        v->msgs[i].md   = t->kind == "plan";  // the plan, in Markdown
        v->askseq = r->seq;
        v->askts  = r->ts;
        v->askmsg = i;
        v->planver     += t->kind == "plan";
        v->asks++;
        i32 c = findcall(v, t->call);
        if (c >= 0) {
            v->msgs[c].tool->status = VTOOL_ASKING;
        }
        clampsel(v);
    } return 1;

    case EV_ANSWER: {
        i32 i = findask(v, jsonstr(jsonget(j, "req")));
        if (i < 0) {
            return 0;
        }
        ViewMsg  *m = &v->msgs[i];
        ViewTool *t = m->tool;
        if (t->answer == VASK_PENDING) {
            v->asks--;
        }
        b32 allow = jsonstr(jsonget(j, "decision")) == "allow";
        t->answer = allow ? VASK_ALLOWED : VASK_DENIED;
        t->by     = clone(&v->aux, jsonstr(jsonget(j, "by")));
        t->note   = clone(&v->aux, jsonstr(jsonget(j, "note")));
        t->always = allow && jsonflag(jsonget(j, "always"), 0);
        t->trusted = allow && jsonflag(jsonget(j, "trust"), 0);
        m->ver++;
        v->topsdirty = 1;  // no more buttons
    } return 1;

    case EV_RETRY: {
        // The model named is the next attempt's: a fallback's after a
        // switch, and until the primary is tried again
        fadeprov(v);
        Arena a     = scratch;
        Buf   b(&a, 256);
        i64   delay = jsonint(jsonget(j, "delay_ms"), 0);
        Str   model = jsonstr(jsonget(j, "model"));
        i32   fb    = (i32)jsonint(jsonget(j, "fallback"), 0);
        b32   slow  = jsonint(jsonget(j, "slow"), 0) != 0;
        v->retrymodel = model==v->retrymodel ? v->retrymodel : clone(&v->aux, model);
        v->retryslow  = slow;
        v->fallback   = fb ? fb : model==v->model ? 0 : v->fallback;
        print(&b, "Attempt ");
        print(&b, jsonint(jsonget(j, "attempt"), 0));
        print(&b, " failed (");
        print(&b, jsonstr(jsonget(j, "class")));
        i64 status = jsonint(jsonget(j, "status"), 0);
        if (status) {
            print(&b, ", HTTP ");
            print(&b, status);
        }
        print(&b, "): ");
        print(&b, jsonstr(jsonget(j, "msg")));
        if (fb) {
            print(&b, ". Switching to fallback ");
            print(&b, (i64)fb);
            print(&b, ", ");
            print(&b, model);
            print(&b, ".");
        } else {
            print(&b, slow ? Str(". Waiting ") : Str(". Retrying in "));
            if (delay < 60000) {
                printfloat(&b, (f64)delay/1000, 1);
                print(&b, " s");
            } else {
                print(&b, delay/60000);
                print(&b, " min");
                if (delay/1000%60) {
                    putbyte(&b, ' ');
                    print(&b, delay/1000%60);
                    print(&b, " s");
                }
            }
            print(&b, slow ? Str(" before trying again (a slow retry)") : Str(""));
            if (model.len && model!=v->model) {
                print(&b, ", with ");
                print(&b, model);
            }
            putbyte(&b, '.');
        }
        i32 i = addmsg(v, VIEW_NOTE, finish(&b));
        v->msgs[i].seq   = r->seq;
        v->msgs[i].label = fb ? Str("Fallback") : Str("Retry");
    } return 1;

    case EV_NUDGE: {
        fadeprov(v);
        i32 i = addmsg(v, VIEW_NOTE, jsonstr(jsonget(j, "text")));
        v->msgs[i].seq   = r->seq;
        v->msgs[i].label = "Reminder to the agent";
    } return 1;

    case EV_NOTE: {
        Str level = jsonstr(jsonget(j, "level"));
        i32 i     = addmsg(v, VIEW_NOTE, jsonstr(jsonget(j, "text")));
        v->msgs[i].seq   = r->seq;
        v->msgs[i].label = level=="warn"  ? Str("Warning") :
                           level=="error" ? Str("Error") : Str{};
    } return 1;

    case EV_NOTICE: {
        // A message the agent reads, folded: what it was told
        Str      kind = jsonstr(jsonget(j, "kind"));
        i64      job  = jsonint(jsonget(j, "job"), 0);
        i32      i    = addmsg(v, VIEW_NOTE, jsonstr(jsonget(j, "text")));
        ViewMsg *m    = &v->msgs[i];
        m->seq       = r->seq;
        m->fold      = 1;
        m->collapsed = 1;
        if (kind=="job" && job) {
            Buf b(&v->aux, 48);
            print(&b, "To the agent: job ");
            print(&b, job);
            print(&b, " ended");
            m->label = finish(&b);
        } else if (kind == "message") {
            // From another session: open, and named as the sender was
            Str from  = jsonstr(jsonget(j, "from"));
            Str title = jsonstr(jsonget(j, "from_title"));
            Str name  = title.len ? title : from.len ? from : Str("another session");
            m->label     = oneline(&v->aux, concat(&scratch, "Message from ", name));
            m->from      = clone(&v->aux, from);
            m->collapsed = 0;
        } else if (kind == "task") {
            // A subagent's report from the background, linked to it
            Str kid   = jsonstr(jsonget(j, "child"));
            Str title = jsonstr(jsonget(j, "child_title"));
            Str name  = title.len ? title : kid.len ? kid : Str("a subagent");
            m->label   = oneline(&v->aux, concat(&scratch, "To the agent: the report of ", name));
            m->from     = clone(&v->aux, kid);
            m->fromkid  = 1;
            m->fromhost = clone(&v->aux, jsonstr(jsonget(j, "host")));
        } else {
            m->label = kind=="rewind"       ? Str("To the agent: files rewound") :
                       kind=="instructions" ? Str("To the agent: instruction files") :
                       kind=="skills"       ? Str("To the agent: skills") :
                       kind=="budget"       ? Str("To the agent: wrap up") :
                       kind=="side"         ? Str("To the agent: a side chat") :
                       kind=="memory"       ? Str("To the agent: memory") :
                       Str("To the agent");
        }
    } return 1;

    case EV_COMPACT: {
        // A card: the summary, or which outputs the agent no longer sees
        Str       kind = jsonstr(jsonget(j, "kind"));
        ViewCard *c    = alloc<ViewCard>(&v->aux);
        c->kind   = kind=="summary" ? Str("summary") : Str("prune");
        Str why   = jsonstr(jsonget(j, "why"));
        c->why    = why=="overflow" ? Str("overflow") :
                    why=="manual"   ? Str("manual")   : Str("threshold");
        c->before = jsonint(jsonget(j, "before"), 0);
        c->after  = jsonint(jsonget(j, "after"), 0);
        c->model  = clone(&v->aux, jsonstr(jsonget(j, "model")));
        c->ms     = jsonint(jsonget(j, "ms"), 0);
        Str text  = {};
        if (kind == "summary") {
            i64 cut = jsonint(jsonget(j, "cutseq"), v->cutseq);
            for (iz i = 0; i < v->msgs.len; i++) {
                ViewMsg *m    = &v->msgs[i];
                b32      talk = m->role==VIEW_USER || m->role==VIEW_ASSISTANT ||
                                m->role==VIEW_THINK || m->role==VIEW_TOOL;
                c->count += talk && m->seq>=v->cutseq && m->seq<cut && m->seq;
            }
            v->cutseq = cut>v->cutseq ? cut : v->cutseq;
            text = jsonstr(jsonget(j, "summary"));
        } else {
            Buf b(&scratch, 1024);
            for (Json *e = jsonget(j, "elided")->child; e; e = e->next) {
                i64       seq  = jsonint(jsonget(e, "seq"), 0);
                Str       what = jsonstr(jsonget(e, "text"));
                ViewTool *t    = 0;
                for (iz i = v->msgs.len-1; i>=0 && !t; i--) {
                    ViewMsg *m = &v->msgs[i];
                    t = m->role==VIEW_TOOL && m->seq==seq ? m->tool : 0;
                }
                print(&b, "- ");
                if (t) {
                    t->elided = 1;
                    print(&b, "**");
                    print(&b, t->name.len ? t->name : Str("call"));
                    print(&b, "** ");
                    codespan(&b, t->summary);
                    print(&b, ": ");
                }
                codespan(&b, what);
                putbyte(&b, '\n');
                c->count++;
            }
            text = finish(&b);
        }
        i32      i = addmsg(v, VIEW_COMPACT, text);
        ViewMsg *m = &v->msgs[i];
        m->seq       = r->seq;
        m->md        = 1;
        m->card      = c;
        m->fold      = 1;
        m->collapsed = 1;
        v->topsdirty = 1;
    } return 1;

    case EV_TODO: {
        Json     *items = jsonget(j, "items");
        iz        n     = jsonlen(items);
        ViewTodo *t     = alloc<ViewTodo>(&v->aux, n);
        iz        k     = 0;
        for (Json *e = items->child; e && k<n; e = e->next, k++) {
            Str st = jsonstr(jsonget(e, "status"));
            t[k].content = clone(&v->aux, jsonstr(jsonget(e, "content")));
            t[k].status  = st=="done"        ? VTODO_DONE   :
                           st=="in_progress" ? VTODO_ACTIVE : VTODO_PENDING;
        }
        v->todos.data = t;
        v->todos.len  = v->todos.cap = k;
    } return 1;

    case EV_JOBSTART: {
        ViewJob jb = {};
        jb.n        = jsonint(jsonget(j, "job"), 0);
        jb.call     = clone(&v->aux, jsonstr(jsonget(j, "call")));
        jb.command  = clone(&v->aux, jsonstr(jsonget(j, "command")));
        jb.summary  = oneline(&v->aux, jb.command);
        jb.spill    = clone(&v->aux, jsonstr(jsonget(j, "spill")));
        jb.start    = r->ts;
        jb.deadline = jsonint(jsonget(j, "deadline"), 0);
        jb.status   = VJOB_RUNNING;
        v->jobs = push(&v->aux, v->jobs, jb);
        v->jobsrun++;
        i32 i = findcall(v, jb.call);
        if (i >= 0) {
            v->msgs[i].tool->job = jb.n;
        }
    } return 1;

    case EV_JOBOUT: {
        ViewJob *jb = findjob(v, jsonint(jsonget(j, "job"), 0));
        if (!jb || jb->status!=VJOB_RUNNING) {
            return 0;  // ended already
        }
        Str tail = jsonstr(jsonget(j, "tail"));
        if (tail.len > jb->tailcap) {
            jb->tailcap = tail.len>4096 ? tail.len : 4096;
            jb->tail    = allocbytes(&v->aux, jb->tailcap);
        }
        copybytes(jb->tail, tail.data, tail.len);
        jb->taillen = tail.len;
        jb->bytes   = jsonint(jsonget(j, "bytes"), jb->bytes);
    } return 1;

    case EV_JOBEND: {
        i64      n  = jsonint(jsonget(j, "job"), 0);
        ViewJob *jb = findjob(v, n);
        if (!jb) {
            ViewJob nj = {};  // its start before what this view saw
            nj.n    = n;
            nj.call = clone(&v->aux, jsonstr(jsonget(j, "call")));
            v->jobs = push(&v->aux, v->jobs, nj);
            jb = &v->jobs[v->jobs.len-1];
        } else if (jb->status == VJOB_RUNNING) {
            v->jobsrun--;
        }
        Json *ex = jsonget(j, "exit");
        jb->status  = vjobstatus(jsonstr(jsonget(j, "status")));
        jb->hasexit = ex->type == JSON_NUMBER;
        jb->exit    = (i32)jsonint(ex, 0);
        jb->signal  = (i32)jsonint(jsonget(j, "signal"), 0);
        jb->bytes   = jsonint(jsonget(j, "bytes"), jb->bytes);
        jb->ms      = jsonint(jsonget(j, "ms"), 0);
        jb->killing = 0;
        Str spill = jsonstr(jsonget(j, "spill"));
        jb->spill = spill.len ? clone(&v->aux, spill) : jb->spill;
    } return 1;

    case EV_SNAP: {
        ViewSnap sn = {};
        sn.call    = clone(&v->aux, jsonstr(jsonget(j, "call")));
        sn.path    = clone(&v->aux, jsonstr(jsonget(j, "path")));
        sn.name    = clone(&v->aux, jsonstr(jsonget(j, "snap")));
        sn.existed = jsonint(jsonget(j, "existed"), 0) != 0;
        sn.seq     = r->seq;
        sn.turn    = v->turn;
        v->snaps = push(&v->aux, v->snaps, sn);
        i32 i = findcall(v, sn.call);
        if (i >= 0) {
            v->msgs[i].tool->snap = (i32)v->snaps.len;
        }
    } return 1;

    case EV_REWIND: {
        // The files it put back are undone, and a card says how each
        // went, with what it left alone
        for (Json *u = jsonget(j, "undone")->child; u; u = u->next) {
            Str name = jsonstr(u);
            for (iz k = 0; k < v->snaps.len; k++) {
                v->snaps[k].undone |= v->snaps[k].name == name;
            }
        }
        for (iz k = 0; k < v->msgs.len; k++) {
            if (v->msgs[k].role==VIEW_REWIND && v->msgs[k].card->latest) {
                v->msgs[k].card->latest = 0;
                v->topsdirty = 1;  // no more buttons on it
            }
        }
        ViewCard *c  = alloc<ViewCard>(&v->aux);
        Json     *to = jsonget(j, "to");
        c->kind  = "rewind";
        c->force = jsonint(jsonget(j, "force"), 0) != 0;
        if (to->type == JSON_STRING) {
            c->to = clone(&v->aux, to->str);
        } else {
            c->turn = (i32)jsonint(to, 0);
        }
        Buf b(&scratch, 1024);
        for (Json *f = jsonget(j, "files")->child; f; f = f->next) {
            Str res = jsonstr(jsonget(f, "result"));
            print(&b, "- ");
            codespan(&b, showpath(v, jsonstr(jsonget(f, "path"))));
            print(&b, res=="restored"  ? Str(": put back as it was") :
                      res=="removed"   ? Str(": removed, as it did not exist then") :
                      res=="unchanged" ? Str(": already as it was") :
                      res=="conflict"  ? Str(": **conflict**, left as it is: something "
                                             "else changed it after the harness did") :
                      res=="partial"   ? Str(": put back **in part**, to before the later "
                                             "changes: something else changed it in "
                                             "between, so the earlier ones stay") :
                      Str(": **failed**, could not be put back"));
            putbyte(&b, '\n');
            c->conflicts += res=="conflict" || res=="partial";
            c->count++;
        }
        Str note = trim(jsonstr(jsonget(j, "note")));
        if (note.len) {
            print(&b, "\n*");  // a footnote
            print(&b, note);
            print(&b, "*\n");
        }
        c->latest = 1;
        i32 i = addmsg(v, VIEW_REWIND, finish(&b));
        v->msgs[i].seq  = r->seq;
        v->msgs[i].md   = 1;
        v->msgs[i].card = c;
    } return 1;

    case EV_INSTRUCTIONS: {
        // Each file once, as last told
        Str why = clone(&v->aux, jsonstr(jsonget(j, "why")));
        for (Json *f = jsonget(j, "files")->child; f; f = f->next) {
            Str        path = jsonstr(jsonget(f, "path"));
            ViewInstr *in   = 0;
            for (iz k = 0; k<v->instr.len && !in; k++) {
                in = v->instr[k].path==path ? &v->instr[k] : 0;
            }
            if (!in) {
                ViewInstr n = {};
                n.path  = clone(&v->aux, path);
                v->instr = push(&v->aux, v->instr, n);
                in = &v->instr[v->instr.len-1];
            }
            in->why      = why;
            in->bytes    = jsonint(jsonget(f, "bytes"), 0);
            in->importer = clone(&v->aux, jsonstr(jsonget(f, "importer")));
        }
        for (Json *s = jsonget(j, "skipped")->child; s; s = s->next) {
            v->skipped = push(&v->aux, v->skipped, clone(&v->aux, jsonstr(s)));
        }
        Str text = jsonstr(jsonget(j, "text"));
        if (text.data) {
            v->instrtext = clone(&v->aux, text);
        }
    } return 1;

    case EV_MEMORY:
        v->memdir = clone(&v->aux, jsonstr(jsonget(j, "dir")));
        v->mems   = viewmems(&v->aux, jsonstr(jsonget(j, "text")));
        return 1;

    case EV_QUEUE: {
        Json       *items = jsonget(j, "items");
        iz          n     = jsonlen(items);
        ViewQueued *q     = alloc<ViewQueued>(&v->aux, n);
        iz          k     = 0;
        for (Json *e = items->child; e && k<n; e = e->next, k++) {
            q[k].text   = clone(&v->aux, jsonstr(jsonget(e, "text")));
            q[k].id     = jsonint(jsonget(e, "id"), 0);
            q[k].steer  = jsonstr(jsonget(e, "kind")) == "steer";
            q[k].cut    = jsonint(jsonget(e, "cut"), 0) > 0;
            q[k].images = (i32)jsonint(jsonget(e, "images"), 0);
        }
        v->queue.data = q;
        v->queue.len  = v->queue.cap = k;
    } return 1;

    case EV_TURNEND: {
        // The queue stays as it was: the engine sends what is left, and
        // a steer too late for this turn stays shown, as a prompt now
        dropprov(v);
        v->doneturn    = (i32)jsonint(jsonget(j, "turn"), v->turn);
        v->doneattempt = 0x7fffffff;
        v->busy  = 0;
        v->state = v->state==LIVE_CLOSED ? LIVE_CLOSED : LIVE_IDLE;
        v->pdone = v->ptotal = 0;
        viewgauge(v, j);
        Str why = jsonstr(jsonget(j, "why"));
        Str err = jsonstr(jsonget(j, "err"));
        v->endseq     = r->seq;
        v->endts      = r->ts;
        v->endwhy     = why=="done"   ? END_DONE   :
                        why=="error"  ? END_ERROR  :
                        why=="budget" ? END_BUDGET : END_OTHER;
        v->enderr     = clone(&v->aux, err);
        v->endresume  = jsonflag(jsonget(j, "resume"), 0);
        v->endcompact = v->turncompact;
        v->turncompact = 0;
        if (why != "done") {
            Arena a = scratch;
            Buf   b(&a, 128 + err.len);
            print(&b, why=="cancel"      ? Str("Cancelled.") :
                      why=="interrupted" ? Str("Interrupted: the harness stopped during this turn.") :
                      why=="budget"      ? Str("Stopped at the step budget.") :
                      why=="side"        ? Str("This side chat began here, while the "
                                               "main session's turn went on there.") :
                      Str("The turn ended with an error."));
            if (err.len) {
                putbyte(&b, ' ');
                print(&b, err);
            }
            i32 i = addmsg(v, VIEW_NOTE, finish(&b));
            v->msgs[i].seq   = r->seq;
            v->msgs[i].label = why=="error" ? Str("Error") :
                               why=="side"  ? Str("Side chat") : Str("Turn ended");
        }
        clampsel(v);
    } return 1;

    case EV_LIVE: {
        // Sent every second while a request runs: only a change worth
        // showing asks for frames, since the clocks have deadlines
        i32  state    = v->state;
        f64  tps      = v->tps;
        i64  retryat  = v->retryat;
        i64  lastdata = v->lastdata;
        i64  watchat  = v->watchat;
        i32  qpos     = v->queuepos;
        i32  qlen     = v->queuelen;
        i32  tasks    = v->tasks;
        b32  novision = v->novision;
        v->state    = livestate(jsonstr(jsonget(j, "state")));
        v->novision = jsonint(jsonget(j, "novision"), 0) != 0;
        v->queuepos = (i32)jsonint(jsonget(j, "queue_pos"), 0);
        v->queuelen = (i32)jsonint(jsonget(j, "queue_len"), 0);
        v->tasks    = (i32)jsonint(jsonget(j, "tasks"), 0);
        Json *idle = jsonget(j, "idle_ms");
        if (idle->type == JSON_NUMBER) {
            v->lastdata = r->ts - jsonint(idle, 0);
        }
        v->tps     = jsonfloat(jsonget(j, "tps"), 0);
        v->retryat = jsonint(jsonget(j, "retry_at"), 0);
        Json *wd   = jsonget(j, "watchdog_ms");
        v->watchat = wd->type==JSON_NUMBER ? r->ts + jsonint(wd, 0) : 0;
        v->watchms = jsonint(jsonget(j, "watchdog_of"), 0);
        i64 moved  = v->lastdata - lastdata;
        i64 moved2 = v->watchat - watchat;
        b32 gauge  = viewgauge(v, j);
        return gauge || state!=v->state || (i64)(tps*10)!=(i64)(v->tps*10) ||
               retryat!=v->retryat || moved<-50 || moved>50 ||
               moved2<-50 || moved2>50 || qpos!=v->queuepos ||
               qlen!=v->queuelen || tasks!=v->tasks || novision!=v->novision;
    }

    case EV_PROGRESS:
        v->lastdata = r->ts;
        v->pdone    = jsonint(jsonget(j, "done"), 0);
        v->ptotal   = jsonint(jsonget(j, "total"), 0);
        return 1;

    case EV_OVERFLOW:
        v->overflow = 1;
        return 1;
    }
    return 0;
}

// Apply one record line.
static b32 viewapply(View *v, Arena scratch, Str line)
{
    EvRec r = evparse(&scratch, line);
    return viewrecord(v, scratch, &r);
}

// The engine stopped (its process died) without ending the session's
// turn, so no record will end it, its calls or its asks: nothing runs
// any more. Calls under way end as unknown, as a log's recovery has
// them, open asks as denied by the stop, and streamed text fades.
static void viewstopped(View *v)
{
    for (iz i = 0; i < v->msgs.len; i++) {
        ViewMsg  *m = &v->msgs[i];
        ViewTool *t = m->tool;
        if (m->role==VIEW_ASK && t->answer==VASK_PENDING) {
            t->answer = VASK_DENIED;
            t->by     = "stopped";
            m->ver++;
        } else if (m->role==VIEW_TOOL && !m->prov &&
                   (t->status==VTOOL_CALLED || t->status==VTOOL_ASKING ||
                    t->status==VTOOL_RUNNING)) {
            t->status = VTOOL_UNKNOWN;
            m->ver++;
        }
    }
    for (iz i = 0; i < v->jobs.len; i++) {
        if (v->jobs[i].status == VJOB_RUNNING) {
            v->jobs[i].status  = VJOB_UNKNOWN;  // its engine took it along
            v->jobs[i].killing = 0;
        }
    }
    fadeprov(v);
    v->queue     = {};
    v->jobsrun   = 0;
    v->busy      = 0;
    v->asks      = 0;
    v->state     = LIVE_CLOSED;
    v->topsdirty = 1;
}


// Documents and layouts, made on demand in the cache

static Doc *msgdoc(View *v, i32 i)
{
    ViewMsg   *m = &v->msgs[i];
    ViewCache *c = v->c;
    if (m->doc && m->docver==m->ver && m->docgen==c->gen) {
        return m->doc;
    }
    switch (m->role) {
    case VIEW_USER:
        m->doc = parseplain(&c->arena, m->text, 0);
        break;
    case VIEW_ASK:
        m->doc = m->md ? parsedoc(&c->arena, m->text, &c->memo)  // a plan
                       : parseplain(&c->arena, m->text, 0);
        break;
    case VIEW_THINK:
    case VIEW_NOTE:
        m->doc = parseplain(&c->arena, m->text, STYLE_DIM);
        break;
    case VIEW_TOOL:
        m->doc = m->md ? parsedoc(&c->arena, m->text, &c->memo)
                       : parseplain(&c->arena, m->text, STYLE_CODE);
        break;
    default:
        m->doc = parsedoc(&c->arena, m->text, &c->memo);
    }
    m->docver = m->ver;
    m->docgen = c->gen;
    return m->doc;
}

struct Thumb {
    f32 w;
    f32 h;
};

// A screenshot's thumbnail: its image fit in the view's box, aspect
// kept, an image pixel at most a unit. Zero for a row without one.
static Thumb msgthumb(View *v, ViewMsg *m)
{
    Thumb r = {};
    if (m->role!=VIEW_TOOL || m->collapsed || !m->tool->image.len ||
            m->tool->imgw<1 || m->tool->imgh<1 || v->thumbw<1 || v->thumbh<1) {
        return r;
    }
    f32 iw = (f32)m->tool->imgw;
    f32 ih = (f32)m->tool->imgh;
    f32 s  = v->thumbw/iw < v->thumbh/ih ? v->thumbw/iw : v->thumbh/ih;
    s   = s<1 ? s : 1;
    r.w = (f32)(i32)(iw*s + 0.5f);
    r.h = (f32)(i32)(ih*s + 0.5f);
    r.w = r.w<1 ? 1 : r.w;
    r.h = r.h<1 ? 1 : r.h;
    return r;
}

// A user's attached image's thumbnail: fit in a quarter of the box.
static Thumb userthumb(View *v, ViewImage *im)
{
    Thumb r  = {};
    f32   bw = v->thumbw / 2;
    f32   bh = v->thumbh / 2;
    if (im->w<1 || im->h<1) {
        r.w = bh;
        r.h = bh;
        return r;
    }
    f32 iw = (f32)im->w;
    f32 ih = (f32)im->h;
    f32 s  = bw/iw < bh/ih ? bw/iw : bh/ih;
    s   = s<1 ? s : 1;
    r.w = (f32)(i32)(iw*s + 0.5f);
    r.h = (f32)(i32)(ih*s + 0.5f);
    r.w = r.w<1 ? 1 : r.w;
    r.h = r.h<1 ? 1 : r.h;
    return r;
}

struct ThumbAt {
    f32 x, y, w, h;
};

// Where a user message's kth thumbnail goes below it, from its left, in
// rows wrapping at the view's width. For k past the last, y is the
// height of them all.
static ThumbAt userthumbat(View *v, ViewMsg *m, iz k)
{
    ThumbAt r    = {};
    f32     rowh = 0;
    for (iz i = 0; i<=k && i<m->images.len; i++) {
        Thumb t = userthumb(v, &m->images[i]);
        if (r.x>0 && r.x+t.w>v->thumbrow) {
            r.x  = 0;
            r.y += rowh + v->gap;
            rowh = 0;
        }
        if (i == k) {
            r.w = t.w;
            r.h = t.h;
            return r;
        }
        r.x += t.w + v->gap;
        rowh = rowh>t.h ? rowh : t.h;
    }
    r.y += rowh;
    return r;
}

// Below a message's content: an open ask's buttons, or the latest
// rewind's, when it left files alone, or a screenshot's thumbnail, or
// the user's attached images'.
static f32 msgfoot(View *v, ViewMsg *m)
{
    if (m->role==VIEW_USER && m->images.len && !m->collapsed) {
        return userthumbat(v, m, m->images.len).y + v->thumbpad;
    }
    b32 ask   = m->role==VIEW_ASK && m->tool->answer==VASK_PENDING;
    b32 force = m->role==VIEW_REWIND && m->card->latest && m->card->conflicts;
    Thumb th  = msgthumb(v, m);
    if (th.h > 0) {
        return th.h + v->thumbpad;
    }
    return (ask || force) && !v->readonly ? v->foot : 0;
}

// Lay out message i at the width, updating its height. A collapsed
// or hidden message has no content.
static MsgLayout *msglayout(View *v, i32 i, Measure *ms, f32 width, f32 scale)
{
    ViewMsg   *m = &v->msgs[i];
    ViewCache *c = v->c;
    if (m->layoutver!=m->ver || m->layoutgen!=c->gen || m->layoutw!=width ||
            !m->measured) {
        if (m->collapsed || msghidden(v, m)) {
            m->layout = {};
            m->layout.width = width;
        } else {
            Doc *d = msgdoc(v, i);
            m->layout = layoutmsg(&c->lcache, &c->arena, ms, d, width, scale);
            v->laidout++;
        }
        m->layoutver = m->ver;
        m->layoutgen = c->gen;
        m->layoutw   = width;
        if (!m->measured || m->h!=m->layout.h) {
            v->topsdirty = 1;
        }
        m->h        = m->layout.h;
        m->measured = 1;
    }
    return &m->layout;
}

// Height estimate for a message not yet laid out: its lines, plus its
// bytes wrapped at a typical character width.
static f32 estimate(ViewMsg *m, f32 width, f32 charw, f32 lineh)
{
    if (m->collapsed) {
        return 0;
    }
    i64 perline = width>charw ? (i64)(width/charw) : 1;
    i64 lines   = m->nl + 1 + m->text.len/perline;
    return (f32)lines * lineh;
}

// Recompute message tops if anything moved. Returns the total height.
static f64 viewtops(View *v, f32 width, f32 charw, f32 lineh)
{
    if (v->topsdirty) {
        f64 y = 0;
        for (iz i = 0; i < v->msgs.len; i++) {
            ViewMsg *m = &v->msgs[i];
            if (!m->measured || m->layoutw!=width) {
                m->measured = 0;
                m->h = estimate(m, width, charw, lineh);
            }
            m->top = y;
            if (!msghidden(v, m)) {
                y += v->extra + m->h + msgfoot(v, m) + v->gap;
            }
        }
        v->topsdirty = 0;
    }
    iz i = v->msgs.len - 1;
    for (; i>=0 && msghidden(v, &v->msgs[i]); i--) {}
    if (i < 0) {
        return 0;
    }
    ViewMsg *last = &v->msgs[i];
    return last->top + v->extra + last->h + msgfoot(v, last);
}

// The message at y: the last whose top is at or above it. A hidden one
// shares its top with the next, so only a run of them at the end could
// be last: then the one shown before them, or after if none, or -1.
static i32 viewfind(View *v, f64 y)
{
    iz lo = 0, hi = v->msgs.len;
    if (!hi) {
        return -1;
    }
    while (hi-lo > 1) {
        iz mid = lo + (hi - lo)/2;
        if (v->msgs[mid].top <= y) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    iz i = lo;
    for (; i>=0 && msghidden(v, &v->msgs[i]); i--) {}
    for (i = i<0 ? lo : i; i<v->msgs.len && msghidden(v, &v->msgs[i]); i++) {}
    return i<v->msgs.len ? (i32)i : -1;
}

// Show or hide a message's content, as the user chose.
static void togglemsg(View *v, i32 i)
{
    ViewMsg *m = &v->msgs[i];
    m->chosen    = 1;
    m->collapsed = !m->collapsed;
    m->ver++;
    v->topsdirty = 1;
}

// What is at a height in the transcript, as a place in the source that a
// new width keeps: the message there, -1 if none; the source offset its
// line there begins at, -1 in its header, a folded body or its foot; and
// how far below that line's top, or the message's, the height is.
struct Anchor {
    i32 msg;
    i32 off;
    f64 dy;
};

// The anchor at y, with the tops and layouts at the width given, below
// a header line of head.
static Anchor viewanchor(View *v, Measure *ms, f32 width, f32 scale, f32 head, f64 y)
{
    Anchor r = {viewfind(v, y), -1, 0};
    if (r.msg < 0) {
        return r;
    }
    ViewMsg *m = &v->msgs[r.msg];
    r.dy = y - m->top;
    f32 ly = (f32)(r.dy - head);
    if (ly<0 || m->collapsed) {
        return r;
    }
    MsgLayout *ml = msglayout(v, r.msg, ms, width, scale);
    if (ly >= ml->h) {
        return r;
    }
    Doc *d = msgdoc(v, r.msg);
    r.off = msghit(ms, d, ml, 0, ly);
    r.dy  = ly - msgy(d, ml, r.off);
    return r;
}

// The height of an anchor at the width given, its message laid out
// there, with the tops as they are.
static f64 anchory(View *v, Anchor an, Measure *ms, f32 width, f32 scale, f32 head)
{
    ViewMsg *m = &v->msgs[an.msg];
    if (an.off<0 || m->collapsed) {
        return m->top + an.dy;
    }
    MsgLayout *ml = msglayout(v, an.msg, ms, width, scale);
    return m->top + head + msgy(msgdoc(v, an.msg), ml, an.off) + an.dy;
}


// Selection

static b32 posless(Pos a, Pos b)
{
    return a.msg<b.msg || (a.msg==b.msg && a.off<b.off);
}

static b32 poseq(Pos a, Pos b)
{
    return a.msg==b.msg && a.off==b.off;
}

struct Range {
    Pos beg;
    Pos end;
};

// The selected range, ordered. Empty if inactive.
static Range selrange(Sel *s)
{
    Range r = {};
    if (!s->active) {
        return r;
    }
    Pos a = s->anchor, b = s->focus;
    if (s->unit != SEL_CHAR) {
        // Extend by whole units: keep the first unit selected whichever
        // way the focus goes.
        Pos a1 = s->anchor1;
        if (posless(b, a)) {
            r.beg = b;
            r.end = a1;
        } else {
            r.beg = a;
            r.end = posless(b, a1) ? a1 : b;
        }
        return r;
    }
    r.beg = posless(b, a) ? b : a;
    r.end = posless(b, a) ? a : b;
    return r;
}

static b32 selempty(Sel *s)
{
    Range r = selrange(s);
    return poseq(r.beg, r.end);
}

static b32 wordbyte(u8 c)
{
    return alnum(c) || c=='_' || c>=0x80;
}

// The word around an offset: letters, digits, underscores and non-ASCII;
// otherwise the single character there.
static Range wordat(View *v, Pos p)
{
    Str s = v->msgs[p.msg].text;
    Range r = {{p.msg, p.off}, {p.msg, p.off}};
    if (p.off>=s.len || !wordbyte(s[p.off])) {
        if (p.off < s.len) {
            iz i = p.off;
            utf8decode(s, &i);
            r.end.off = (i32)i;
        }
        return r;
    }
    i32 b = p.off, e = p.off;
    for (; b>0 && wordbyte(s[b-1]); b--) {}
    for (; e<s.len && wordbyte(s[e]); e++) {}
    // Keep whole UTF-8 sequences: never start on a continuation byte.
    for (; b<p.off && (s[b]&0xc0)==0x80; b++) {}
    r.beg.off = b;
    r.end.off = e;
    return r;
}

// Index of the block holding a source offset, or the one before it.
static i32 blockat(Doc *d, i32 off)
{
    iz lo = 0, hi = d->blocks.len;
    if (!hi) {
        return -1;
    }
    while (hi-lo > 1) {
        iz mid = lo + (hi - lo)/2;
        if (d->blocks[mid].off0 <= off) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return (i32)lo;
}

static Range blockrange(View *v, Pos p)
{
    Range r = {{p.msg, 0}, {p.msg, (i32)v->msgs[p.msg].text.len}};
    Doc  *d = msgdoc(v, p.msg);
    i32   b = blockat(d, p.off);
    if (b >= 0) {
        r.beg.off = d->blocks[b].off0;
        r.end.off = d->blocks[b].off1;
    }
    return r;
}

// Begin a selection at p with the given unit (click, double, triple).
static void selbegin(View *v, Pos p, i32 unit)
{
    Sel *s = &v->sel;
    s->active = 1;
    s->unit   = unit;
    Range r = unit==SEL_WORD  ? wordat(v, p) :
              unit==SEL_BLOCK ? blockrange(v, p) :
              Range{p, p};
    s->anchor  = r.beg;
    s->anchor1 = r.end;
    s->focus   = r.end;
}

// Extend the selection to p, by the selection's unit.
static void selextend(View *v, Pos p)
{
    Sel *s = &v->sel;
    if (!s->active) {
        selbegin(v, p, SEL_CHAR);
        return;
    }
    if (s->unit == SEL_CHAR) {
        s->focus = p;
        return;
    }
    Range r = s->unit==SEL_WORD ? wordat(v, p) : blockrange(v, p);
    s->focus = posless(p, s->anchor) ? r.beg : r.end;
}

static void selall(View *v)
{
    if (!v->msgs.len) {
        return;
    }
    i32 last = (i32)v->msgs.len - 1;
    v->sel.active  = 1;
    v->sel.unit    = SEL_CHAR;
    v->sel.anchor  = {0, 0};
    v->sel.anchor1 = {0, 0};
    v->sel.focus   = {last, (i32)v->msgs[last].text.len};
}


// Copying

// Text a piece contributes to the source range [s, e).
static Str piececlip(Piece *p, i32 s, i32 e)
{
    if (p->flags & PIECE_SYNTH) {
        return p->off>=s && p->off<e ? p->text : Str{};
    } else if (p->len != p->text.len) {
        return p->off<e && p->off+p->len>s ? p->text : Str{};
    }
    i32 b = p->off>s ? p->off : s;
    i32 x = p->off+p->len<e ? p->off+p->len : e;
    if (b >= x) {
        return {};
    }
    return {p->text.data + (b - p->off), x - b};
}

// What goes between two blocks in copied text.
static Str blocksep(DocBlock *prev, DocBlock *b)
{
    if (prev->type==DOC_CELL && b->type==DOC_CELL && prev->table==b->table) {
        return prev->row==b->row ? Str("\t") : Str("\n");
    } else if (prev->type==DOC_PLAIN && b->type==DOC_PLAIN) {
        return "\n";
    } else if (b->type==DOC_ITEM && (prev->type==DOC_ITEM || prev->depth)) {
        return "\n";
    }
    return "\n\n";
}

// The selected part [s, e) of a message as plain text: piece texts, code
// byte-exact, with list markers and block separators restored.
static void copytext(Buf *b, Doc *d, i32 s, i32 e)
{
    DocBlock *prev = 0;
    for (iz i = 0; i < d->blocks.len; i++) {
        DocBlock *k = &d->blocks[i];
        if (k->off1<s || k->off0>e) {
            continue;
        }
        iz  before = b->len;
        b32 starts = s<=k->off0 && k->off0<e;  // markers belong to starts
        if (prev) {
            Str sep = blocksep(prev, k);
            if (b->len && b->data[b->len-1]=='\n' && sep.len && sep[0]=='\n') {
                sep = cuthead(sep, 1);  // code already ended its line
            }
            print(b, sep);
        }
        b32 any = 0;
        if (k->item && starts) {
            for (i32 n = 1; n < k->depth; n++) print(b, "  ");
            if (k->task) {
                print(b, k->task==2 ? Str("- [x] ") : Str("- [ ] "));
            } else if (k->number < 0) {
                print(b, "- ");
            } else {
                print(b, (i64)k->number);
                print(b, ". ");
            }
            any = 1;
        }
        if (k->type==DOC_HR && starts) {
            print(b, "---");
            any = 1;
        }
        if (!k->npiece && starts) {
            any = 1;  // an empty line or item
        }
        for (i32 j = 0; j < k->npiece; j++) {
            Str t = piececlip(&d->pieces[k->piece0+j], s, e);
            print(b, t);
            any |= t.len > 0;
        }
        if (!any) {
            b->len = before;  // nothing of this block was selected
            continue;
        }
        prev = k;
    }
}

struct Range32 {
    i32 beg;
    i32 end;
};

static i32 linestart(Str s, i32 off)
{
    for (; off>0 && s[off-1]!='\n'; off--) {}
    return off;
}

// Whether a line is a code fence, past any container prefix.
static b32 fenceline(Str line)
{
    iz i = 0;
    for (; i<line.len && (line[i]==' ' || line[i]=='>' || line[i]=='\t'); i++) {}
    Str t = cuthead(line, i);
    return startswith(t, "```") || startswith(t, "~~~");
}

// The source lines of block i, with a code block's fences.
static Range32 blocklines(Doc *d, iz i)
{
    Str       src = d->src;
    DocBlock *k   = &d->blocks[i];
    Range32   r   = {linestart(src, k->off0), lineend(src, k->off1)};
    if (k->type==DOC_CODE && k->fence) {
        if (k->npiece && r.beg>0) {
            r.beg = linestart(src, r.beg-1);  // the opening fence
        }
        if (r.end < src.len) {
            i32 ne = lineend(src, r.end+1);
            if (fenceline(span(src.data+r.end+1, src.data+ne))) {
                r.end = ne;  // the closing fence
            }
        }
    }
    return r;
}

// The selected part [s, e) of a message as markdown: the source itself
// within one block, else whole blocks' source lines.
static void copymarkdown(Buf *b, Doc *d, i32 s, i32 e)
{
    Str src = d->src;
    i32 bs  = blockat(d, s);
    i32 be  = blockat(d, e>s ? e-1 : e);
    if (bs<0 || bs==be) {
        print(b, span(src.data+s, src.data+e));
        return;
    }
    Range32 first = blocklines(d, bs);
    Range32 last  = blocklines(d, be);
    i32 beg = first.beg<s ? first.beg : s;
    i32 end = last.end>e ? last.end : e;
    print(b, span(src.data+beg, src.data+end));
}

enum : i32 {
    COPY_TEXT,
    COPY_MARKDOWN,
};

// The selection as text or markdown; messages are separated by a blank
// line.
static Str copysel(View *v, Arena *perm, i32 how)
{
    Range r = selrange(&v->sel);
    if (!v->sel.active || poseq(r.beg, r.end)) {
        return {};
    }
    Buf b(perm, 1<<12);
    for (i32 i = r.beg.msg; i <= r.end.msg; i++) {
        ViewMsg *m = &v->msgs[i];
        i32  s = i==r.beg.msg ? r.beg.off : 0;
        i32  e = i==r.end.msg ? r.end.off : (i32)m->text.len;
        if (i > r.beg.msg) {
            print(&b, "\n\n");
        }
        Doc *d = msgdoc(v, i);
        if (how == COPY_MARKDOWN) {
            copymarkdown(&b, d, s, e);
        } else {
            copytext(&b, d, s, e);
        }
    }
    return finish(&b);
}


// A code block's code, as selecting the block whole (a triple click)
// copies it: byte for byte, but for its last newline, so that a command
// pasted into a terminal waits for Enter.
static Str copyblock(View *v, Arena *perm, i32 msg, i32 block)
{
    Doc      *d = msgdoc(v, msg);
    DocBlock *k = &d->blocks[block];
    Buf       b(perm, k->off1 - k->off0 + 64);
    copytext(&b, d, k->off0, k->off1);
    return finish(&b);
}


// Searching

struct FindHit {
    i32 msg;
    i32 off;
    i32 len;
};

// A message as a search last saw it.
struct FindSeen {
    u8 *data;
    iz  len;
    i32 ver;
    b32 hidden;
};

struct ViewSearch {
    View            *v;
    Str              q;         // lowercase ASCII
    iz               qmax;
    Arena            hitmem;    // each alone in its arena, to grow in place
    byte            *hitbase;
    Arena            seenmem;
    byte            *seenbase;
    Slice<FindHit>   hits;      // in order
    Slice<FindSeen>  seen;      // the messages searched, in order
    i32              cur;       // the current hit, -1 if none
};

static ViewSearch *newsearch(Arena *perm, Region *hits, Region *seen, iz qmax)
{
    ViewSearch *s = alloc<ViewSearch>(perm);
    s->hitmem   = newarena(hits);
    s->hitbase  = s->hitmem.beg;
    s->seenmem  = newarena(seen);
    s->seenbase = s->seenmem.beg;
    s->q.data   = alloc<u8>(perm, qmax);
    s->qmax     = qmax;
    s->cur      = -1;
    return s;
}

// Search a view for q from the start, as searchstep goes on to.
static void searchreset(ViewSearch *s, View *v, Str q)
{
    q = q.len>s->qmax ? takehead(q, s->qmax) : q;
    for (iz i = 0; i < q.len; i++) {
        s->q.data[i] = lowercase(q[i]);
    }
    s->q.len      = q.len;
    s->v          = v;
    s->hitmem.beg = s->hitbase;
    s->seenmem.beg = s->seenbase;
    s->hits       = {};
    s->seen       = {};
    s->cur        = -1;
}

static FindSeen seenof(View *v, ViewMsg *m)
{
    return {m->text.data, m->text.len, m->ver, msghidden(v, m)};
}

static b32 seeneq(FindSeen a, FindSeen b)
{
    return a.data==b.data && a.len==b.len && a.ver==b.ver && a.hidden==b.hidden;
}

// Bring the hits up to date with the view: searched again from the first
// message that changed, at most about budget bytes of text at a time.
// Returns whether they are, else it goes on from there next call.
static b32 searchstep(ViewSearch *s, iz budget)
{
    View *v = s->v;
    iz    n = s->seen.len<v->msgs.len ? s->seen.len : v->msgs.len;
    iz    first = n;
    for (iz i = 0; i < n; i++) {
        if (!seeneq(s->seen[i], seenof(v, &v->msgs[i]))) {
            first = i;
            break;
        }
    }
    if (first < s->seen.len) {
        iz k = s->hits.len;
        for (; k>0 && s->hits[k-1].msg>=first; k--) {}
        s->hits.len = k;
        s->seen.len = first;
        s->cur      = s->cur<k ? s->cur : (i32)k-1;
    }

    Str q = s->q;
    for (iz i = s->seen.len; i < v->msgs.len; i++) {
        if (budget <= 0) {
            return 0;
        }
        ViewMsg *m = &v->msgs[i];
        Str      t = m->text;
        s->seen = push(&s->seenmem, s->seen, seenof(v, m));
        if (!q.len || msghidden(v, m)) {
            continue;
        }
        budget -= t.len + 1;
        u8 q0 = q[0];
        for (iz at = 0; at+q.len <= t.len; at++) {
            if (lowercase(t[at]) != q0) {
                continue;
            }
            iz k = 1;
            for (; k<q.len && lowercase(t[at+k])==q[k]; k++) {}
            if (k == q.len) {
                FindHit h = {(i32)i, (i32)at, (i32)q.len};
                s->hits = push(&s->hitmem, s->hits, h);
                at += q.len - 1;
            }
        }
    }
    return 1;
}

// The first hit in a message at or after an offset, else the next
// message's, else hits.len.
static iz searchfrom(ViewSearch *s, i32 msg, i32 off)
{
    iz lo = 0, hi = s->hits.len;
    while (lo < hi) {
        iz mid = lo + (hi - lo)/2;
        FindHit *h = &s->hits[mid];
        if (h->msg<msg || (h->msg==msg && h->off+h->len<=off)) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

// Show a hit: its message opened if folded, as if the user had, which
// changes nothing to search again.
static void searchreveal(ViewSearch *s, FindHit *h)
{
    View *v = s->v;
    if (v->msgs[h->msg].collapsed) {
        togglemsg(v, h->msg);
        if (h->msg < s->seen.len) {
            s->seen[h->msg].ver = v->msgs[h->msg].ver;
        }
    }
}


// Comparing views: what two views show as text, for the resync check.

static void viewdump(Buf *b, View *v)
{
    for (iz i = 0; i < v->msgs.len; i++) {
        ViewMsg *m = &v->msgs[i];
        if (m->prov) {
            continue;
        }
        print(b, (i64)m->role);
        putbyte(b, ' ');
        print(b, m->seq);
        putbyte(b, ' ');
        print(b, m->label);
        if (m->tool) {
            ViewTool *t = m->tool;
            putbyte(b, ' ');
            print(b, t->name);
            putbyte(b, ' ');
            print(b, (i64)t->status);
            putbyte(b, ' ');
            print(b, (i64)t->answer);
            putbyte(b, ' ');
            print(b, t->by);
        }
        putbyte(b, ':');
        print(b, m->text);
        putbyte(b, '\n');
    }
    print(b, "busy ");
    print(b, (i64)v->busy);
    print(b, " lastseq ");
    print(b, v->lastseq);
    print(b, " title ");
    print(b, v->title);
    putbyte(b, '\n');
    for (iz i = 0; i < v->todos.len; i++) {
        print(b, "todo ");
        print(b, (i64)v->todos[i].status);
        putbyte(b, ' ');
        print(b, v->todos[i].content);
        putbyte(b, '\n');
    }
    for (iz i = 0; i < v->jobs.len; i++) {
        print(b, "job ");
        print(b, v->jobs[i].n);
        putbyte(b, ' ');
        print(b, vjobnames[v->jobs[i].status]);
        putbyte(b, '\n');
    }
    for (iz i = 0; i < v->snaps.len; i++) {
        print(b, "snap ");
        print(b, v->snaps[i].name);
        print(b, v->snaps[i].undone ? Str(" undone\n") : Str("\n"));
    }
    for (iz i = 0; i < v->instr.len; i++) {
        print(b, "instructions ");
        print(b, v->instr[i].path);
        putbyte(b, '\n');
    }
}


// Exports

// A fence of backticks longer than any run in s, at least three.
static void exportfence(Buf *b, Str s)
{
    iz n = backticks(s) + 1;
    for (iz i = 0; i < (n>3 ? n : 3); i++) {
        putbyte(b, '`');
    }
}

// The first line of s, trimmed and cut to a summary's length, and
// whether anything was left out.
static Str exportline(Str s, b32 *more)
{
    s = trim(s);
    Cut c = cut(s, '\n');
    Str r = trimright(c.head);
    *more = c.ok;
    if (r.len > VIEW_SUMMARY) {
        iz n = VIEW_SUMMARY;
        for (; n>0 && (r[n]&0xc0)==0x80; n--) {}
        r     = takehead(r, n);
        *more = 1;
    }
    return r;
}

// The transcript as a Markdown document, for an export: the title and
// what the session is, then each message under a heading of who wrote
// it. Reasoning (think), tool calls and asks as a line of their name
// and summary (tools), and under each call its output (output) are left
// out unless asked for. Notes and cards are a line each, in italics.
static void viewexport(Buf *b, View *v, Str sid, b32 think, b32 tools, b32 output)
{
    print(b, "# ");
    print(b, v->title.len ? v->title : Str("Untitled session"));
    print(b, "\n\nSession ");
    print(b, sid);
    if (v->model.len) {
        print(b, ", ");
        print(b, v->model);
    }
    if (v->nofolder) {
        print(b, ", with no folder");
    } else if (v->cwd.len) {
        print(b, ", in ");
        codespan(b, v->cwd);
    }
    putbyte(b, '\n');
    i32 last = -1;
    for (iz i = 0; i < v->msgs.len; i++) {
        ViewMsg *m    = &v->msgs[i];
        b32      more = 0;
        if (m->prov) {
            continue;
        }
        switch (m->role) {
        case VIEW_USER:
        case VIEW_ASSISTANT:
            if (m->role==VIEW_USER || last!=VIEW_THINK) {
                print(b, "\n## ");
                print(b, m->label.len ? m->label :
                         m->role==VIEW_USER ? Str("You") : Str("Agent"));
                putbyte(b, '\n');
            }
            putbyte(b, '\n');
            print(b, m->text);
            print(b, m->text.len && m->text[m->text.len-1]=='\n' ? Str("") : Str("\n"));
            for (iz k = 0; k < m->images.len; k++) {
                print(b, k ? Str(" [image]") : Str("\n[image]"));
            }
            print(b, m->images.len ? Str("\n") : Str(""));
            break;
        case VIEW_THINK:
            if (!think || !m->text.len) {
                continue;
            }
            print(b, last==VIEW_THINK ? Str("") : Str("\n## Agent\n"));
            print(b, "\n> **Reasoning**\n>\n");
            for (Str rest = m->text; rest.len;) {
                Cut c = cut(rest, '\n');
                print(b, c.head.len ? Str("> ") : Str(">"));
                print(b, c.head);
                putbyte(b, '\n');
                rest = c.tail;
            }
            break;
        case VIEW_TOOL:
        case VIEW_ASK: {
            ViewTool *t = m->tool;
            if (!tools || !t) {
                continue;
            }
            b32 ask = m->role == VIEW_ASK;
            print(b, ask ? Str("\n**Asked: ") : Str("\n**Tool: "));
            print(b, ask ? t->tool : t->name);
            print(b, "**");
            Str what = exportline(ask ? (t->path.len ? t->path : t->detail) : t->summary, &more);
            if (what.len) {
                putbyte(b, ' ');
                codespan(b, what);
                print(b, more ? Str("\xe2\x80\xa6") : Str(""));
            }
            if (ask) {
                print(b, t->trusted                ? Str(" (trusted)") :
                         t->always                 ? Str(" (allowed always)") :
                         t->answer==VASK_ALLOWED ? Str(" (allowed)") :
                         t->answer==VASK_DENIED  ? Str(" (denied)")  : Str(""));
            } else if (t->status != VTOOL_OK) {
                print(b, " (");
                print(b, vtoolnames[t->status]);
                putbyte(b, ')');
            }
            putbyte(b, '\n');
            if (output && !ask && m->text.len) {
                putbyte(b, '\n');
                exportfence(b, m->text);
                putbyte(b, '\n');
                print(b, m->text);
                print(b, m->text[m->text.len-1]=='\n' ? Str("") : Str("\n"));
                exportfence(b, m->text);
                putbyte(b, '\n');
            }
        } break;
        default: {
            // Notes, compactions, rewinds: their first line
            Str line = exportline(m->text, &more);
            if (!line.len) {
                continue;
            }
            print(b, "\n_");
            print(b, m->label.len ? m->label : m->role==VIEW_COMPACT ? Str("Compacted")
                                             : m->role==VIEW_REWIND  ? Str("Rewound")
                                                                     : Str("Note"));
            print(b, ":_ ");
            print(b, line);
            print(b, more ? Str("\xe2\x80\xa6\n") : Str("\n"));
        }
        }
        last = m->role;
    }
}

// Desktop notifications ([ui] notify): the events a list of words names,
// as NOTIFY_* bits, read left to right, each word adding its event and,
// with a leading "-", taking it away again. "none" names nothing, and is
// there so that a list may say so (an empty one means the default).
// Unknown words are left out (config.cpp warns).
enum : u32 {
    NOTIFY_ALLOW = 1<<0,  // an ask waits for an answer
    NOTIFY_DONE  = 1<<1,  // a session stopped by itself
};

static u32 notifyparse(Str words)
{
    u32 r = 0;
    for (Str rest = words; rest.len;) {
        Str w    = cfgnextword(&rest);
        b32 less = w.len>1 && w[0]=='-';
        w = cfglistword(w);
        u32 bit = w=="allow" ? (u32)NOTIFY_ALLOW : w=="done" ? (u32)NOTIFY_DONE : 0;
        r = less ? r&~bit : r|bit;
    }
    return r;
}

// How well a setting matches a Settings search, best first: 1 its key
// is the word, 2 the key starts with it, 3 a part of the key after "_",
// "." or "-" starts with it, 4 the key has it anywhere, 5 its page's
// label (kind, section name or General's heading) has it, 6 its
// description does. Case doesn't count. Every word of the query must
// match, and the weakest match is the rank. 0 is no match, as is an
// empty query.
static i32 cfgfindrank(Str key, Str desc, Str page, Str query)
{
    i32 rank = 0;
    for (Str rest = query; rest.len;) {
        Str w = cfgnextword(&rest);
        if (!w.len) {
            continue;
        }
        i32 r   = 0;
        iz  at  = findfold(key, w);
        if (at < 0) {
            r = findfold(page, w)>=0 ? 5 : findfold(desc, w)>=0 ? 6 : 0;
        } else if (w.len == key.len) {
            r = 1;
        } else if (at == 0) {
            r = 2;
        } else {
            r = 4;
            for (iz i = 1; i+w.len <= key.len; i++) {
                u8 c = key[i-1];
                if ((c=='_' || c=='.' || c=='-') &&
                    equalfold(Str{key.data+i, w.len}, w)) {
                    r = 3;
                    break;
                }
            }
        }
        if (!r) {
            return 0;
        }
        rank = r>rank ? r : rank;
    }
    return rank;
}

// A notification's title for a session: its title, else the one listed
// for it, else its folder's name, else the application's.
static Str toasttitle(Arena *a, View *v, Str listed)
{
    Str dir = v->cwd;
    for (; dir.len>1 && (dir[dir.len-1]=='/' || dir[dir.len-1]=='\\'); dir.len--) {}
    Str t = v->title.len  ? v->title :
            listed.len    ? listed   :
            !v->nofolder  ? basename(dir) : Str{};
    return t.len ? oneline(a, t) : Str("Prospero");
}

// What an ask wants, as its card's header says it (ui.cpp).
static Str askheadline(Arena *a, ViewTool *t)
{
    if (t->kind == "plan") {
        return "Approve the plan?";
    } else if (t->kind == "plan.enter") {
        return "Plan first, changing nothing until you approve?";
    }
    Buf b(a, 64 + t->tool.len + t->path.len);
    if (t->kind == "mcp.project") {
        print(&b, "Start the MCP servers of ");
        print(&b, t->path);
    } else {
        print(&b, "Allow ");
        print(&b, t->tool);
        if (t->path.len) {
            print(&b, " ");
            print(&b, t->path);
        }
    }
    print(&b, "?");
    return oneline(a, finish(&b));
}

// A notification's text for the asks waiting in a session's tree: one,
// t, from the session named who (a subagent's, else empty), at epoch ms
// now; or how many.
static Str toastaskbody(Arena *a, ViewTool *t, Str who, i32 pending, i64 now)
{
    Buf b(a, 512);
    if (pending > 1) {
        print(&b, (i64)pending);
        print(&b, " requests wait for your answer");
        return finish(&b);
    }
    if (who.len) {
        print(&b, who);
        print(&b, ": ");
    }
    print(&b, askheadline(a, t));
    if (!t->deadline) {
        print(&b, "\nWaiting for you");
    } else {
        i64 secs = (t->deadline - now + 999) / 1000;
        print(&b, "\nDenied in ");
        print(&b, secs>0 ? secs : 0);
        print(&b, " s unless allowed");
    }
    return finish(&b);
}

// A notification's text for a session that stopped by itself (endwhy):
// the first line of its last reply, or why it stopped.
static Str toastdonebody(Arena *a, View *v)
{
    switch (v->endwhy) {
    case END_ERROR:
        return v->enderr.len ? concat(a, "Stopped: ", oneline(a, v->enderr))
                             : Str("Stopped by an error");
    case END_BUDGET:
        return "Stopped at the step budget";
    }
    for (iz i = v->msgs.len-1; i >= 0; i--) {
        ViewMsg *m = &v->msgs[i];
        if (m->role == VIEW_USER) {
            break;  // nothing said since the prompt
        } else if (m->role==VIEW_ASSISTANT && trim(m->text).len) {
            return oneline(a, m->text);
        }
    }
    return "Finished";
}

// s cut in its middle to fit in width, measured in style: whole if it
// fits, else "…" between as much of its head and its tail as fits,
// taken a code point at a time from each end in turn, tail first, as
// what tells a branch or a path from another is as often at its end.
// Empty if not even a code point of each fits around the "…".
static Str midcut(Arena *a, Str s, f32 width, Measure *m, i32 style)
{
    f32 w = 0;
    for (iz i = 0; i < s.len;) {
        w += m->advance(m->ctx, style, utf8decode(s, &i));
    }
    if (w <= width) {
        return s;
    }
    f32 room = width - m->advance(m->ctx, style, 0x2026);
    iz  head = 0;     // s[0, head) kept
    iz  tail = s.len; // and s[tail, len)
    for (b32 back = 1; head < tail; back = !back) {
        iz  at = head;
        i32 cp = back ? utf8prev(s, tail, &at) : utf8decode(s, &at);
        f32 cw = m->advance(m->ctx, style, cp);
        if (cw>room || (back ? at<head : at>tail)) {
            break;
        }
        room -= cw;
        if (back) {
            tail = at;
        } else {
            head = at;
        }
    }
    if (!head || tail==s.len) {
        return {};
    }
    Buf b(a, head + 3 + s.len - tail);
    print(&b, takehead(s, head));
    print(&b, "\xe2\x80\xa6");
    print(&b, cuthead(s, tail));
    return finish(&b);
}

// A session's branch as its engine last read it (git.cpp githead), and
// the GUI's asking for it and for its changes (gitnext).
enum : i32 {
    GITW_UNKNOWN,  // not yet asked
    GITW_NONE,     // not in a repository
    GITW_REPO,
};

struct GitWatch {
    i32 state;     // GITW_*
    Str root;
    Str branch;    // else the whole ref, unless reftable or detached
    Str oid;       // detached at
    Str op;        // in progress: "rebase", "merge", "am", ...
    i64 step;      // its step of steps, if it counts them
    i64 steps;
    b32 detached;
    b32 reftable;  // HEAD names no branch git's files can tell
    b32 stale;     // it may have changed since last asked
    i64 id;        // the request in flight, else 0
    i64 sent;      // when the last went, epoch ms
    i64 statusat;  // no status sooner (gitstatusat)
};

enum : i32 {
    GITASK_NONE,
    GITASK_HEAD,    // the branch
    GITASK_STATUS,  // and the changes
};

enum : i64 {
    GITW_LOST   = 30*1000,  // a request unanswered this long was lost
    GITW_GAP    = 1000,     // the least time between a session's requests
    GITW_MAXGAP = 60*1000,  // the most between its status requests
};

struct GitNext {
    i32 ask;   // GITASK_*
    i64 wake;  // else when to look again, epoch ms, or 0 for no sooner
               // than the next change
};

// What to ask a session's engine as of now (epoch ms): its branch, or
// its changes too (status) while the Changes pane shows it. Only once
// they may have changed (stale), with its own last request answered or
// lost, once a second at most, and with no status in flight for
// another session (busy: when that one went, else 0). A status waits
// out the gap its last one asked for (statusat) only if the pane has
// that one to show meanwhile (have).
static GitNext gitnext(GitWatch *w, b32 status, b32 have, i64 busy, i64 now)
{
    GitNext r = {};
    if (!w->stale) {
        return r;
    }
    i64 at = w->sent + GITW_GAP;
    if (w->id) {
        at = at>w->sent+GITW_LOST ? at : w->sent+GITW_LOST;
    }
    if (status && have) {
        at = at>w->statusat ? at : w->statusat;
    }
    if (status && busy) {
        at = at>busy+GITW_LOST ? at : busy+GITW_LOST;
    }
    if (now < at) {
        r.wake = at;
    } else {
        r.ask = status ? GITASK_STATUS : GITASK_HEAD;
    }
    return r;
}

// When a session may ask for its changes again after a reply to the
// request sent at sent: five times what git took (ms) later, from 1 s
// to 60 s, or 60 s after a failure, which asking again soon won't fix.
static i64 gitstatusat(i64 sent, i64 ms, b32 ok)
{
    i64 gap = ok ? 5*ms : GITW_MAXGAP;
    gap = gap<GITW_GAP ? GITW_GAP : gap>GITW_MAXGAP ? GITW_MAXGAP : gap;
    return sent + gap;
}
