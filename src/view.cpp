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
// This is free and unencumbered software released into the public domain.

enum : i32 {
    VIEW_USER,
    VIEW_ASSISTANT,
    VIEW_THINK,
    VIEW_TOOL,
    VIEW_NOTE,
    VIEW_ASK,
};

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
    VTOOL_COUNT,
};

static Str vtoolnames[] = {
    "called", "streaming", "asking", "running", "ok", "error", "denied",
    "timeout", "cancelled", "unknown",
};
static_assert(countof(vtoolnames) == VTOOL_COUNT);

// Asks
enum : i32 {
    VASK_PENDING,
    VASK_ALLOWED,
    VASK_DENIED,
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
    LIVE_CLOSED,
    LIVE_COUNT,
};

static Str livenames[] = {
    "idle", "discover", "waiting", "streaming", "error", "backoff", "tool",
    "ask", "closed",
};
static_assert(countof(livenames) == LIVE_COUNT);

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
    i64  ms;        // its duration, once ended
    Str  spill;     // the full output, relative to the session directory
    i64  bytes;
    i64  outbytes;  // output so far, while running
    u8  *tail;      // the live output, a buffer reused for each tool.out
    iz   tailcap;
    // Asks
    Str  req;
    Str  tool;      // the tool that asked
    Str  path;
    Str  detail;
    Str  hint;
    i64  deadline;  // epoch ms
    i32  answer;    // VASK_*
    Str  by;
    Str  note;
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
    b32       md;         // markdown, not plain text
    Str       label;      // the header, if not the role's name
    ViewTool *tool;       // tool rows and asks
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
    Sel             sel;
    i64             deltas;     // delta records applied, for rates
    i64             deltabytes;
    i64             laidout;    // messages laid out, for benchmarks
    b32             overflow;   // fake agent: events were dropped

    // Ordering
    i64             lastseq;    // the last durable record applied
    b32             lost;       // records went missing: resync from the log
    b32             loose;      // a replay: seq may skip numbers
    i32             doneturn;   // the last committed response:
    i32             doneattempt;  // deltas up to it are stale

    // The session
    Str             title;
    Str             cwd;
    Str             model;
    Str             endpoint;
    i32             turn;
    i32             attempt;
    b32             busy;       // a turn is running
    b32             readonly;   // open elsewhere: answering is not possible
    i64             turnts;     // epoch ms the running turn began
    i32             asks;       // pending asks

    // Liveness, from live and progress records (epoch ms)
    i32             state;      // LIVE_*
    i64             lastdata;   // the model last sent anything
    f64             tps;
    i64             retryat;    // backing off until then
    i64             watchat;    // the nearest watchdog fires then, 0 if none
    i64             watchms;    // and waits this long in all
    i64             pdone;      // prompt processing, tokens of total
    i64             ptotal;
    i64             outts;      // the last tool.out
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

static i64 countnl(Str s)
{
    i64 n = 0;
    for (iz i = 0; i < s.len; i++) {
        n += s.data[i] == '\n';
    }
    return n;
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
    v->msgs = push(&v->meta, v->msgs, m);
    v->topsdirty  = 1;
    v->provmixed |= v->prov >= 0;
    v->stream     = -1;  // later deltas start a new message after it
    return (i32)v->msgs.len - 1;
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
    v->msgs = push(&v->meta, v->msgs, m);
    v->topsdirty = 1;
    return (i32)v->msgs.len - 1;
}

// Append to the provisional message for this kind of delta, starting
// one if needed. Its text is the text arena's last allocation, so this
// extends it in place.
static void viewdelta(View *v, i32 role, Str text)
{
    if (v->stream<0 || v->streamkind!=role) {
        v->stream     = addprov(v, role);
        v->streamkind = role;
    }
    ViewMsg *m = &v->msgs[v->stream];
    m->text = concat(&v->text, m->text, text);
    m->nl  += countnl(text);
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
        v->msgs[i].collapsed = 1;
        v->stream = -1;  // text after it is a new message
    }
    if (id.len)   t->call = clone(&v->aux, id);
    if (name.len) t->name = clone(&v->aux, name);
    t->args = concat(&v->aux, t->args, text);
    v->deltas++;
    v->deltabytes += text.len;
}

// Drop the provisional messages, which a committed record replaces.
// Their text is reclaimed unless other messages came in between.
static void dropprov(View *v)
{
    if (v->prov >= 0) {
        iz n = v->prov;
        for (iz i = v->prov; i < v->msgs.len; i++) {
            if (!v->msgs[i].prov) {
                v->msgs[n++] = v->msgs[i];
            }
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

// What a call is about, from its arguments: the command or the path.
static Str toolsummary(Arena *perm, Arena scratch, Str name, Str args)
{
    Json *j   = jsonparse(&scratch, args).root;
    Str   cmd = jsonstr(jsonget(j, "command"));
    Str   p   = jsonstr(jsonget(j, "path"));
    if (name=="shell" && cmd.data) {
        return oneline(perm, cmd);
    } else if (p.data) {
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
    Json *j    = jsonparse(a, args).root;
    Str   path = jsonstr(jsonget(j, "path"));
    Buf   d(a, 1<<12);
    if (name == "edit") {
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

// A committed call's row.
static i32 addcall(View *v, Arena scratch, Str id, Str name, Str args)
{
    i32       i = addmsg(v, VIEW_TOOL, "");
    ViewTool *t = alloc<ViewTool>(&v->aux);
    t->call    = clone(&v->aux, id);
    t->name    = clone(&v->aux, name);
    t->args    = clone(&v->aux, args);
    t->summary = toolsummary(&v->aux, scratch, name, args);
    t->status  = VTOOL_CALLED;
    v->msgs[i].tool      = t;
    v->msgs[i].collapsed = name=="read" || name=="write";
    return i;
}

// The body of a finished call: a diff for edits and writes that worked,
// otherwise the result the agent read, then where the whole output is.
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
    if (t->spill.len) {
        Buf b(&scratch, text.len + 256);
        print(&b, text);
        if (text.len && text[text.len-1]!='\n') putbyte(&b, '\n');
        print(&b, "\nFull output: ");
        print(&b, v->dir.len ? join(&scratch, v->dir, t->spill) : t->spill);
        print(&b, " (");
        print(&b, t->bytes);
        print(&b, " bytes)");
        text = finish(&b);
    }
    settext(v, m, text, 0);
}


// Records

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
        v->model    = clone(&v->aux, jsonstr(jsonget(j, "model")));
        v->endpoint = clone(&v->aux, jsonstr(jsonget(j, "endpoint")));
        if (v->state == LIVE_CLOSED) {
            v->state = LIVE_IDLE;
        }
        return 1;

    case EV_TITLE:
        v->title = clone(&v->aux, jsonstr(jsonget(j, "text")));
        return 1;

    case EV_USER: {
        dropprov(v);
        Str via = jsonstr(jsonget(j, "via"));
        i32 i   = addmsg(v, VIEW_USER, jsonstr(jsonget(j, "text")));
        v->msgs[i].seq   = r->seq;
        v->msgs[i].label = via=="steer" ? Str("You (steering)") :
                           via=="queue" ? Str("You (queued)") : Str{};
    } return 1;

    case EV_TURN:
        v->turn    = (i32)jsonint(jsonget(j, "n"), v->turn+1);
        v->attempt = 0;
        v->busy    = 1;
        v->turnts  = r->ts;
        v->pdone   = v->ptotal = 0;
        return 1;

    case EV_ATTEMPT:
        dropprov(v);  // a failed attempt's, shown faded until now
        v->attempt = (i32)jsonint(jsonget(j, "n"), v->attempt+1);
        v->pdone   = v->ptotal = 0;
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
            viewdelta(v, kind=="think" ? VIEW_THINK : VIEW_ASSISTANT, text);
        }
    } return 1;

    case EV_ASSISTANT: {
        dropprov(v);
        v->doneturn    = (i32)jsonint(jsonget(j, "turn"), v->turn);
        v->doneattempt = (i32)jsonint(jsonget(j, "attempt"), v->attempt);
        Json *blocks = jsonget(j, "blocks");
        for (Json *b = blocks->child; b; b = b->next) {
            Str t = jsonstr(jsonget(b, "t"));
            i32 i = -1;
            if (t == "think") {
                i = addmsg(v, VIEW_THINK, jsonstr(jsonget(b, "text")));
                v->msgs[i].collapsed = 1;
            } else if (t == "text") {
                i = addmsg(v, VIEW_ASSISTANT, jsonstr(jsonget(b, "text")));
            } else if (t == "call") {
                i = addcall(v, scratch, jsonstr(jsonget(b, "id")),
                            jsonstr(jsonget(b, "name")), jsonstr(jsonget(b, "args")));
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
        m->tool->status = VTOOL_RUNNING;
        m->tool->start  = r->ts;
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
        m->seq     = r->seq;
        toolbody(v, scratch, m, jsonstr(jsonget(j, "text")));
        if (t->status != VTOOL_OK) {
            m->collapsed = 0;  // what went wrong
        } else if (t->name == "shell") {
            m->collapsed = m->nl > 24;
        }
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
        t->call     = clone(&v->aux, jsonstr(jsonget(j, "call")));
        t->tool     = clone(&v->aux, jsonstr(jsonget(j, "tool")));
        t->path     = clone(&v->aux, jsonstr(jsonget(j, "path")));
        t->detail   = clone(&v->aux, detail);
        t->hint     = clone(&v->aux, hint);
        t->deadline = jsonint(jsonget(j, "deadline"), r->ts);
        v->msgs[i].tool = t;
        v->msgs[i].seq  = r->seq;
        v->asks++;
        i32 c = findcall(v, t->call);
        if (c >= 0) {
            v->msgs[c].tool->status = VTOOL_ASKING;
        }
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
        m->ver++;
        v->topsdirty = 1;  // no more buttons
    } return 1;

    case EV_RETRY: {
        fadeprov(v);
        Arena a = scratch;
        Buf   b(&a, 256);
        i64   delay = jsonint(jsonget(j, "delay_ms"), 0);
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
        print(&b, ". Retrying in ");
        printfloat(&b, (f64)delay/1000, 1);
        print(&b, " s.");
        i32 i = addmsg(v, VIEW_NOTE, finish(&b));
        v->msgs[i].seq   = r->seq;
        v->msgs[i].label = "Retry";
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

    case EV_TURNEND: {
        dropprov(v);
        v->doneturn    = (i32)jsonint(jsonget(j, "turn"), v->turn);
        v->doneattempt = 0x7fffffff;
        v->busy  = 0;
        v->state = v->state==LIVE_CLOSED ? LIVE_CLOSED : LIVE_IDLE;
        v->pdone = v->ptotal = 0;
        Str why = jsonstr(jsonget(j, "why"));
        Str err = jsonstr(jsonget(j, "err"));
        if (why != "done") {
            Arena a = scratch;
            Buf   b(&a, 128 + err.len);
            print(&b, why=="cancel"      ? Str("Cancelled.") :
                      why=="interrupted" ? Str("Interrupted: the harness stopped during this turn.") :
                      why=="budget"      ? Str("Stopped at the step budget.") :
                      Str("The turn ended with an error."));
            if (err.len) {
                putbyte(&b, ' ');
                print(&b, err);
            }
            i32 i = addmsg(v, VIEW_NOTE, finish(&b));
            v->msgs[i].seq   = r->seq;
            v->msgs[i].label = why=="error" ? Str("Error") : Str("Turn ended");
        }
        clampsel(v);
    } return 1;

    case EV_LIVE: {
        v->state = livestate(jsonstr(jsonget(j, "state")));
        Json *idle = jsonget(j, "idle_ms");
        if (idle->type == JSON_NUMBER) {
            v->lastdata = r->ts - jsonint(idle, 0);
        }
        v->tps     = jsonfloat(jsonget(j, "tps"), 0);
        v->retryat = jsonint(jsonget(j, "retry_at"), 0);
        Json *wd   = jsonget(j, "watchdog_ms");
        v->watchat = wd->type==JSON_NUMBER ? r->ts + jsonint(wd, 0) : 0;
        v->watchms = jsonint(jsonget(j, "watchdog_of"), 0);
    } return 1;

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
    case VIEW_ASK:
        m->doc = parseplain(&c->arena, m->text, 0);
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

// Below a message's content: an open ask's buttons.
static f32 msgfoot(View *v, ViewMsg *m)
{
    b32 open = m->role==VIEW_ASK && m->tool->answer==VASK_PENDING && !v->readonly;
    return open ? v->foot : 0;
}

// Lay out message i at the width, updating its height. A collapsed
// message has no content.
static MsgLayout *msglayout(View *v, i32 i, Measure *ms, f32 width, f32 scale)
{
    ViewMsg   *m = &v->msgs[i];
    ViewCache *c = v->c;
    if (m->layoutver!=m->ver || m->layoutgen!=c->gen || m->layoutw!=width ||
            !m->measured) {
        if (m->collapsed) {
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
            y += v->extra + m->h + msgfoot(v, m) + v->gap;
        }
        v->topsdirty = 0;
    }
    if (!v->msgs.len) {
        return 0;
    }
    ViewMsg *last = &v->msgs[v->msgs.len-1];
    return last->top + v->extra + last->h + msgfoot(v, last);
}

// The message at y: the last whose top is at or above it.
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
    return (i32)lo;
}

// Show or hide a message's content.
static void togglemsg(View *v, i32 i)
{
    ViewMsg *m = &v->msgs[i];
    m->collapsed = !m->collapsed;
    m->ver++;
    v->topsdirty = 1;
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
}
