// Protocol v0 records and session event logs
//
// A record is one JSON object on one line ending in \n, with keys in a
// fixed order: sid, seq (durable records only), ts, ev, then the payload.
// Commands from the GUI start with cmd and id instead. A session's log,
// events.jsonl, holds exactly its durable records, byte for byte as they
// are sent to the GUI, so replaying a session means streaming its file.
// The protocol only grows: an engine's hello names its commands (cmds)
// and features (caps), so that a frontend offers only what it has.
//
// Logs are appended one write per record, so a software crash loses
// nothing and a power loss at worst tears the final line. Opening a log
// repairs that tail and then closes out whatever the crash left open:
// tools that started but never ended, and the turn they ran in. Records
// of events this version does not know (from a newer one) still hold
// their seq and replay as they are, so no number is ever used twice.
//
// This is free and unencumbered software released into the public domain.

enum : i32 {
    EV_NONE,  // unknown or absent
    // Durable: logged and replayed
    EV_SESSION,
    EV_TITLE,
    EV_USER,
    EV_TURN,
    EV_ATTEMPT,
    EV_ASSISTANT,
    EV_TOOLSTART,
    EV_TOOLEND,
    EV_ASK,
    EV_ANSWER,
    EV_RETRY,
    EV_NUDGE,
    EV_TURNEND,
    EV_NOTE,
    EV_COMPACT,
    EV_JOBSTART,
    EV_JOBEND,
    EV_TODO,
    EV_SNAP,
    EV_REWIND,
    EV_INSTRUCTIONS,
    EV_NOTICE,
    EV_PREFIX,
    EV_MODEL,   // (M8) the session's model from here on
    EV_GRANT,   // (M8) an "always allow" from the user
    EV_MODE,    // plan mode, Bypass mode, or neither, from here on
    // Transient: sent, never logged
    EV_DELTA,
    EV_PROGRESS,
    EV_TOOLOUT,
    EV_LIVE,
    EV_SNAPSHOT,
    EV_SESSIONS,
    EV_OVERFLOW,
    EV_HELLO,
    EV_REPLY,
    EV_JOBOUT,
    EV_REMOTE,  // (M5) a client library's connection to its host
    EV_DEPLOY,  // (M5) progress deploying an engine to a remote machine
    EV_QUEUE,   // what the user sent that waits to be taken in
    EV_COUNT,
};

struct EvInfo {
    Str name;
    b32 durable;
    b32 sync;  // fsync after appending
};

static EvInfo evinfo[] = {
    {"",           0, 0},
    {"session",    1, 0},
    {"title",      1, 0},
    {"user",       1, 0},
    {"turn",       1, 0},
    {"attempt",    1, 0},
    {"assistant",  1, 0},
    {"tool.start", 1, 1},  // before the tool runs
    {"tool.end",   1, 0},
    {"ask",        1, 1},
    {"answer",     1, 1},
    {"retry",      1, 0},
    {"nudge",      1, 0},
    {"turn.end",   1, 1},
    {"note",       1, 0},
    {"compact",    1, 0},
    {"job.start",  1, 0},
    {"job.end",    1, 0},
    {"todo",       1, 0},
    {"snap",       1, 0},
    {"rewind",     1, 1},  // files were changed back
    {"instructions", 1, 0},
    {"notice",     1, 0},
    {"prefix",     1, 0},  // what every request of the session begins with
    {"model",      1, 0},
    {"grant",      1, 0},  // lost to a power cut, it is asked again
    {"mode",       1, 0},
    {"delta",      0, 0},
    {"progress",   0, 0},
    {"tool.out",   0, 0},
    {"live",       0, 0},
    {"snapshot",   0, 0},
    {"sessions",   0, 0},
    {"overflow",   0, 0},
    {"hello",      0, 0},
    {"reply",      0, 0},
    {"job.out",    0, 0},
    {"remote",     0, 0},
    {"deploy",     0, 0},
    {"queue",      0, 0},
};
static_assert(countof(evinfo) == EV_COUNT);

enum : i32 {
    CMD_NONE,  // unknown or absent
    CMD_HELLO,
    CMD_LIST,
    CMD_NEW,
    CMD_OPEN,
    CMD_CLOSE,
    CMD_PROMPT,
    CMD_STEER,
    CMD_CANCEL,
    CMD_ANSWER,
    CMD_CONFIGGET,
    CMD_CONFIGSET,
    CMD_QUIT,
    CMD_REWIND,
    CMD_JOBKILL,
    CMD_STATUS,  // (M5) whether anything runs: for a host's idle exit
    CMD_STOP,    // (M5) stop every turn and job, running on
    CMD_FILE,    // (M5) part of a file in a session's directory
    CMD_RENAME,  // (M8) a title from the user
    CMD_DELETE,  // (M8) a session and its subagents, for good
    CMD_ARCHIVE, // (M8) out of the way, or back
    CMD_GROUP,   // (M8) into a named group, or out of one
    CMD_FORK,    // (M8) a new session from a copy of the log
    CMD_COMPACT, // (M8) compact now
    CMD_MODEL,   // (M8) another model from here on
    CMD_DRAFT,   // (M8) the frontend's unsent prompt, kept
    CMD_UPLOAD,  // (M9) part of a file staged for import
    CMD_IMPORT,  // (M9) a new session from a log staged by upload
    CMD_SKILLS,  // a session's skills, for /name
    CMD_MODE,    // plan mode, Bypass mode, or neither
    CMD_UNQUEUE, // withdraw a queued message not yet taken in
    CMD_DIR,     // a directory's entries, for completing a path typed
    CMD_BACKGROUND,  // the running command or subagents, to the background
    CMD_MODELS,  // the models each endpoint lists, and the defaults
    CMD_COUNT,
};

static Str cmdnames[] = {
    "", "hello", "list", "new", "open", "close", "prompt", "steer",
    "cancel", "answer", "config.get", "config.set", "quit", "rewind",
    "job.kill", "status", "stop", "file", "rename", "delete", "archive",
    "group", "fork", "compact", "model", "draft", "upload", "import",
    "skills", "mode",
    "unqueue", "dir", "background", "models",
};
static_assert(countof(cmdnames) == CMD_COUNT);

// What hello's caps name: features of the engine that its commands
// alone do not show, which a frontend checks for before it relies on
// one, since an older engine lacks it. Names are only ever added.
static Str capnames[] = {
    "close.forget",    // close {forget}: out of memory once idle
    "config.refresh",  // config.get {refresh}: URL includes fetched anew
    "file.base64",     // file {encoding:"base64"}, for binary files
    "desktop",         // the desktop tool, with screenshots in tool.end
    "subagents",       // the task tool: sessions listed under a parent
    // Commands named in cmds since the protocol froze, as each works
    "rename",          // rename
    "title.by",        // title {by}: "user" (rename) or "auto"
    "archive",         // archive, and list's archived
    "group",           // group, and list's group
    "draft",           // draft, and open's draft
    "compact",         // compact, a turn of its own (via "compact")
    "model",           // model, and the durable model record
    "gauge",           // ctx, compact_at and tokens on attempt, turn.end, live
    "answer.always",   // answer {decision:"always"} for an ask's always
    "scratch",         // a scratch directory per session, and [guard] temp
    "delete",          // delete, a session with its subagents
    "fork",            // fork, a new session from a turn of another
    "config.delete",   // config.set {delete}: a section of the user's file
    "import",          // upload and import, a session from a raw log
    "answer.always.desktop",  // a desktop ask's always, by action class
    "skills",          // skills, the skill tool, and prompt {skill}
    "plan",            // plan mode: mode, new {plan}, the mode record, asks
                       // of kind plan and plan.enter, model while asked
    "queue",           // queue records, and unqueue to withdraw from one
    "message",         // sessions and send_message, notices of kind message
    "images",          // upload {kind:"image"}, prompt and steer {images}
    "home",            // hello {home}, and new and import take a cwd
                       // beginning with ~, in that home directory
    "bypass",          // mode {mode:"bypass"}, the mode record's bypass,
                       // and answers by "bypass"
    "nofolder",        // new and import {nofolder}: a folder of the session's
                       // own, and nofolder in its session record and list
    "dir",             // dir, a directory's entries on the engine's machine
    "config.create",   // config.set {create}: the user's file, made if missing
    "side",            // fork {side:1}, a side chat also of a busy session,
                       // and side_of in its session record and list
    "background",      // background: the user moves a running command to a
                       // job, or subagents on, their reports as notices
                       // of kind task (turn via "task")
    "mcp",             // MCP servers' tools, asks of kind mcp whose always
                       // is the tool, grants by mcp, and the state "mcp"
    "models",          // models, each endpoint's listing and the defaults;
                       // new and import split ENDPOINT/MODEL, and default
                       // to the first endpoint and model there are
};

static Str evname(i32 ev)
{
    assert(ev>=0 && ev<EV_COUNT);
    return evinfo[ev].name;
}

static i32 evfind(Str name)  // EV_NONE if unknown
{
    for (i32 ev = 1; ev < EV_COUNT; ev++) {
        if (evinfo[ev].name == name) {
            return ev;
        }
    }
    return EV_NONE;
}

static b32 evdurable(i32 ev)
{
    assert(ev>=0 && ev<EV_COUNT);
    return evinfo[ev].durable;
}

static Str cmdname(i32 cmd)
{
    assert(cmd>=0 && cmd<CMD_COUNT);
    return cmdnames[cmd];
}

static i32 cmdfind(Str name)  // CMD_NONE if unknown
{
    for (i32 cmd = 1; cmd < CMD_COUNT; cmd++) {
        if (cmdnames[cmd] == name) {
            return cmd;
        }
    }
    return CMD_NONE;
}

// A session id as a command names it: one directory under sessions/,
// never a path, a dot file, nor a drive or stream (':') on Windows.
static b32 validsid(Str sid)
{
    b32 ok = sid.len && sid.len<=64 && sid[0]!='.';
    for (iz i = 0; ok && i < sid.len; i++) {
        u8 c = sid[i];
        ok = c>=0x20 && c!=0x7f && c!='/' && c!='\\' && c!=':';
    }
    return ok;
}


// Records

// A record being written: add payload fields through json, then evend.
struct EvOut {
    JsonOut json;
    iz      start;  // offset of the record in the buffer
    i32     ev;     // EV_NONE for commands
};

// Begin an event record. The seq is written only for durable events, and
// a null sid is omitted (for replies outside any session).
static EvOut evbegin(Buf *b, Str sid, i64 seq, i64 ts, i32 ev)
{
    assert(ev>EV_NONE && ev<EV_COUNT);
    EvOut r = {};
    r.json.b = b;
    r.start  = b->len;
    r.ev     = ev;
    putobject(&r.json);
    if (sid.data) {
        putkey(&r.json, "sid");
        putstr(&r.json, sid);
    }
    if (evinfo[ev].durable) {
        putkey(&r.json, "seq");
        putint(&r.json, seq);
    }
    putkey(&r.json, "ts");
    putint(&r.json, ts);
    putkey(&r.json, "ev");
    putstr(&r.json, evinfo[ev].name);
    return r;
}

// Begin a command record (GUI to engine): cmd, id, then the payload.
static EvOut cmdbegin(Buf *b, i32 cmd, i64 id)
{
    assert(cmd>CMD_NONE && cmd<CMD_COUNT);
    EvOut r = {};
    r.json.b = b;
    r.start  = b->len;
    putobject(&r.json);
    putkey(&r.json, "cmd");
    putstr(&r.json, cmdnames[cmd]);
    putkey(&r.json, "id");
    putint(&r.json, id);
    return r;
}

// Close the record and end its line, returning the whole record.
static Str evend(EvOut *r)
{
    assert(r->json.depth == 1);
    putend(&r->json);
    putbyte(r->json.b, '\n');
    return cuthead(finish(r->json.b), r->start);
}

// What hello says the engine speaks: cmds, every command it takes, and
// caps (capnames), with "host" from an engine behind a host (host.cpp),
// but drop, which a test names to stand in for an older engine's.
static void putcaps(JsonOut *w, b32 host, Str drop = {})
{
    putkey(w, "cmds");
    putarray(w);
    for (i32 cmd = 1; cmd < CMD_COUNT; cmd++) {
        putstr(w, cmdnames[cmd]);
    }
    putend(w);
    putkey(w, "caps");
    putarray(w);
    for (iz i = 0; i < countof(capnames); i++) {
        if (!(capnames[i] == drop)) {
            putstr(w, capnames[i]);
        }
    }
    if (host) {
        putstr(w, "host");
    }
    putend(w);
}

// Whether version a is before b, each as numbers between dots, compared
// in turn (missing ones are 0), anything after the numbers ignored.
static b32 versionless(Str a, Str b)
{
    for (i32 part = 0; part < 8; part++) {
        i64 x = 0, y = 0;
        for (; a.len && digit(a[0]); a = cuthead(a, 1)) x = x*10 + (a[0]-'0');
        for (; b.len && digit(b[0]); b = cuthead(b, 1)) y = y*10 + (b[0]-'0');
        if (x != y) {
            return x < y;
        }
        a = a.len && a[0]=='.' ? cuthead(a, 1) : Str{};
        b = b.len && b[0]=='.' ? cuthead(b, 1) : Str{};
        if (!a.len && !b.len) {
            break;
        }
    }
    return 0;
}

// Whether an engine whose hello said version and caps is older than
// this program's: an earlier version, or of this one (as every build
// from source is "0.0.0") without a feature this one has.
static b32 engineolder(Str version, Slice<Str> caps)
{
    if (versionless(version, PROSPERO_VERSION)) {
        return 1;
    } else if (versionless(PROSPERO_VERSION, version)) {
        return 0;
    }
    for (iz i = 0; i < countof(capnames); i++) {
        b32 has = 0;
        for (iz k = 0; k < caps.len && !has; k++) {
            has = caps[k] == capnames[i];
        }
        if (!has) {
            return 1;
        }
    }
    return 0;
}

// A record read back.
struct EvRec {
    Json *root;
    Str   sid;  // null if absent
    i64   seq;  // 0 if absent
    i64   ts;
    i32   ev;   // EV_NONE if absent or unknown
    b32   ok;   // a JSON object
};

static EvRec evparse(Arena *perm, Str line)
{
    EvRec   r = {};
    JsonDoc d = jsonparse(perm, line);
    r.root = d.root;
    r.sid  = jsonstr(jsonget(d.root, "sid"));
    r.seq  = jsonint(jsonget(d.root, "seq"), 0);
    r.ts   = jsonint(jsonget(d.root, "ts"), 0);
    r.ev   = evfind(jsonstr(jsonget(d.root, "ev")));
    r.ok   = d.root->type == JSON_OBJECT;
    return r;
}

// A line belongs in the log of session sid after seq last: an object
// with that sid and a later seq, whatever its event, since judging by
// known events would let the next record reuse an unknown one's seq.
// Gaps (left by invalid lines) are limited, so garbage cannot push seq
// to overflow.
static b32 evvalid(EvRec *r, Str sid, i64 last)
{
    i64 maxgap = (i64)1 << 32;
    return r->ok && r->sid==sid && r->seq>last && r->seq-last<=maxgap;
}


// Session logs

struct EventLog {
    Os  *os;
    Str  sid;
    iz   fd;     // os_logopen handle, -1 when closed
    i64  seq;    // last sequence number, so the next is seq+1
    i64  turn;   // last turn number found at open
    iz   torn;   // bytes of torn tail dropped at open
    i32  bad;    // invalid lines skipped at open
    i32  synth;  // crash-recovery records appended at open
    b32  ok;
};

// Begin a durable record numbered after the last one in the log. Finish
// it with logappend before beginning another.
static EvOut logbegin(EventLog *log, Buf *b, i32 ev)
{
    assert(evdurable(ev));
    return evbegin(b, log->sid, log->seq+1, os_time(log->os), ev);
}

// Finish a record from logbegin and append it with a single write,
// syncing after the events that must survive a power loss. Returns the
// line (to send on to the GUI), or a null string on failure, after
// which the session should stop writing.
static Str logappend(EventLog *log, EvOut *r)
{
    Str line = evend(r);
    if (log->fd<0 || !os_logwrite(log->os, log->fd, line)) {
        return {};
    }
    log->seq++;
    if (evinfo[r->ev].sync && !os_logsync(log->os, log->fd)) {
        return {};
    }
    return line;
}

static void logclose(EventLog *log)
{
    if (log->fd >= 0) {
        os_logclose(log->os, log->fd);
        log->fd = -1;
    }
}

// An ask without an answer yet, while reading a log.
struct LogAsk {
    Str req;   // raw JSON text, as in the log
    Str call;
};

// Remove a raw call id from a list, if there.
static void logforget(Slice<Str> *list, Str call)
{
    for (iz i = 0; i < list->len; i++) {
        if ((*list)[i] == call) {
            for (iz j = i+1; j < list->len; j++) {
                (*list)[j-1] = (*list)[j];
            }
            list->len--;
            return;
        }
    }
}

// Open a session log for appending, creating it if missing. A torn final
// line is truncated away, or ended with a newline if the log cannot be
// truncated. Lines that are not records of this session in increasing
// seq order are skipped (and counted). Then whatever a crash left open
// is closed, in the order a running session would have logged it:
// every tool.start without a tool.end gets tool.end{status:"unknown"};
// each call of the last assistant record that never started gets the
// answer to its ask if one was pending (denied, by "cancel"), then
// tool.start and tool.end{status:"cancelled"}; every job.start without
// a job.end gets job.end{status:"unknown"}; and last, an open turn gets
// turn.end{why:"interrupted"}. Reading needs max bytes of scratch
// at most (a larger log fails), plus parsing space for one line.
static EventLog logopen(Os *os, Arena *perm, Arena scratch, Str path,
                        Str sid, iz max)
{
    EventLog r = {};
    r.os  = os;
    r.sid = clone(perm, sid);
    r.fd  = -1;

    Read rd = os_read(os, &scratch, path, max);
    if (rd.err!=READ_OK && rd.err!=READ_NOTFOUND) {
        return r;
    }
    r.fd = os_logopen(os, scratch, path);
    if (r.fd < 0) {
        return r;
    }

    Str data = rd.data;
    iz  end  = data.len;
    for (; end>0 && data[end-1]!='\n'; end--) {}
    if (end < data.len) {
        // Cut the torn tail away, or where that is not allowed (an
        // append-only file), end it so that it stands as the last line,
        // judged below as any later open will judge it.
        if (os_logtrunc(os, r.fd, end)) {
            r.torn = data.len - end;
            data   = takehead(data, end);
        } else if (!os_logwrite(os, r.fd, "\n")) {
            logclose(&r);
            return r;
        }
    }

    // Track open tools, asks and results by the raw text of their ids
    // (the writer's escaping is canonical), and the last assistant record
    // by its line, all of which point into data and so outlive the parse
    // of each line. Each push follows the last use of that line's parse.
    Slice<Str>    tools    = {};  // started, not ended
    Slice<Str>    ended    = {};  // since the last assistant record
    Slice<LogAsk> asks     = {};  // not answered
    Slice<LogAsk> jobs     = {};  // started, not ended: {job, call}
    Str           asst     = {};  // the last assistant record
    Str           cwd      = {};  // raw, from the session record
    b32           turnopen = 0;
    for (Str rest = data; rest.len;) {
        Cut c = cut(rest, '\n');
        rest = c.tail;

        Arena tmp = scratch;
        EvRec rec = evparse(&tmp, c.head);
        if (!evvalid(&rec, r.sid, r.seq)) {
            r.bad++;
            continue;
        }
        r.seq = rec.seq;

        Json *id   = jsonget(rec.root, "call");
        Str   call = id->type==JSON_STRING ? id->raw : Str{};
        Json *rq   = jsonget(rec.root, "req");
        Str   req  = rq->type==JSON_STRING ? rq->raw : Str{};
        switch (rec.ev) {
        case EV_SESSION: {
            Json *dir = jsonget(rec.root, "cwd");
            cwd = dir->type==JSON_STRING ? dir->raw : Str{};
        } break;
        case EV_TURN:
            r.turn   = jsonint(jsonget(rec.root, "n"), r.turn);
            turnopen = 1;
            break;
        case EV_TURNEND:
            turnopen = 0;
            break;
        case EV_ASSISTANT:
            asst      = c.head;
            ended.len = 0;
            break;
        case EV_TOOLSTART:
            if (call.data) {
                tools = push(&scratch, tools, call);  // tmp is dead now
            }
            break;
        case EV_TOOLEND:
            logforget(&tools, call);
            if (call.data) {
                ended = push(&scratch, ended, call);
            }
            break;
        case EV_ASK:
            if (req.data && call.data) {
                LogAsk k = {req, call};
                asks = push(&scratch, asks, k);
            }
            break;
        case EV_ANSWER:
            for (iz i = 0; i < asks.len; i++) {
                if (asks[i].req == req) {
                    asks[i] = asks[asks.len-1];
                    asks.len--;
                    break;
                }
            }
            break;
        case EV_JOBSTART: {
            Json *n = jsonget(rec.root, "job");
            if (n->type==JSON_NUMBER && call.data) {
                LogAsk k = {n->raw, call};
                jobs = push(&scratch, jobs, k);
            }
        } break;
        case EV_JOBEND: {
            Str n = jsonget(rec.root, "job")->raw;
            for (iz i = 0; i < jobs.len; i++) {
                if (jobs[i].req == n) {
                    for (iz k = i+1; k < jobs.len; k++) {
                        jobs[k-1] = jobs[k];
                    }
                    jobs.len--;
                    break;
                }
            }
        } break;
        }
    }

    // The calls of the last assistant record that never started
    Slice<Json *> unstarted = {};
    if (asst.data) {
        EvRec rec = evparse(&scratch, asst);
        for (Json *k = jsonget(rec.root, "blocks")->child; k; k = k->next) {
            Json *cid  = jsonget(k, "id");
            Str   call = cid->type==JSON_STRING ? cid->raw : Str{};
            if (jsonstr(jsonget(k, "t"))!="call" || !call.data) {
                continue;
            }
            b32 seen = 0;
            for (iz i = 0; i<tools.len && !seen; i++) {
                seen = tools[i] == call;
            }
            for (iz i = 0; i<ended.len && !seen; i++) {
                seen = ended[i] == call;
            }
            if (!seen) {
                unstarted = push(&scratch, unstarted, k);
            }
        }
    }

    Buf b(&scratch, 1<<10);
    for (iz i = 0; i < tools.len; i++) {
        b.len = 0;
        EvOut w = logbegin(&r, &b, EV_TOOLEND);
        putkey(&w.json, "call");
        putraw(&w.json, tools[i]);
        putkey(&w.json, "status");
        putstr(&w.json, "unknown");
        putkey(&w.json, "text");
        putstr(&w.json, "outcome unknown: the harness stopped while this ran");
        if (!logappend(&r, &w).data) {
            logclose(&r);
            return r;
        }
        r.synth++;
    }

    for (iz i = 0; i < unstarted.len; i++) {
        Json *k    = unstarted[i];
        Str   call = jsonget(k, "id")->raw;
        for (iz n = 0; n < asks.len; n++) {
            if (!(asks[n].call == call)) {
                continue;
            }
            b.len = 0;
            EvOut w = logbegin(&r, &b, EV_ANSWER);
            putkey(&w.json, "req");
            putraw(&w.json, asks[n].req);
            putkey(&w.json, "decision");
            putstr(&w.json, "deny");
            putkey(&w.json, "by");
            putstr(&w.json, "cancel");
            putkey(&w.json, "note");
            putstr(&w.json, "the harness stopped while asking");
            if (!logappend(&r, &w).data) {
                logclose(&r);
                return r;
            }
            r.synth++;
        }

        b.len = 0;
        EvOut w = logbegin(&r, &b, EV_TOOLSTART);
        putkey(&w.json, "call");
        putraw(&w.json, call);
        putkey(&w.json, "name");
        putstr(&w.json, jsonstr(jsonget(k, "name")));
        putkey(&w.json, "args");
        putstr(&w.json, jsonstr(jsonget(k, "args")));
        putkey(&w.json, "cwd");
        if (cwd.data) {
            putraw(&w.json, cwd);
        } else {
            putstr(&w.json, "");
        }
        if (!logappend(&r, &w).data) {
            logclose(&r);
            return r;
        }
        r.synth++;

        b.len = 0;
        w = logbegin(&r, &b, EV_TOOLEND);
        putkey(&w.json, "call");
        putraw(&w.json, call);
        putkey(&w.json, "status");
        putstr(&w.json, "cancelled");
        putkey(&w.json, "text");
        putstr(&w.json, "Not run: the harness stopped before this call started.");
        if (!logappend(&r, &w).data) {
            logclose(&r);
            return r;
        }
        r.synth++;
    }

    // Background jobs died with the harness, by the same means as its
    // tools: their reapers' lifeline, or their job objects closing
    for (iz i = 0; i < jobs.len; i++) {
        b.len = 0;
        EvOut w = logbegin(&r, &b, EV_JOBEND);
        putkey(&w.json, "job");
        putraw(&w.json, jobs[i].req);
        putkey(&w.json, "call");
        putraw(&w.json, jobs[i].call);
        putkey(&w.json, "status");
        putstr(&w.json, "unknown");
        putkey(&w.json, "text");
        putstr(&w.json, "the harness stopped while this job ran, which "
                        "stopped the job too");
        if (!logappend(&r, &w).data) {
            logclose(&r);
            return r;
        }
        r.synth++;
    }

    if (turnopen) {
        b.len = 0;
        EvOut w = logbegin(&r, &b, EV_TURNEND);
        putkey(&w.json, "turn");
        putint(&w.json, r.turn);
        putkey(&w.json, "why");
        putstr(&w.json, "interrupted");
        if (!logappend(&r, &w).data) {
            logclose(&r);
            return r;
        }
        r.synth++;
    } else if (r.synth && !os_logsync(os, r.fd)) {
        logclose(&r);
        return r;
    }

    r.ok = 1;
    return r;
}

struct Replay {
    Slice<Str> lines;  // each a complete record with its newline
    i64        last;   // seq of the last valid record, 0 if none
    i64        end;    // file offset past the last complete line
    b32        ok;
};

// Read the durable records with seq > from, as logopen would accept
// them, without modifying the log: a torn tail and invalid lines are
// skipped, while unknown events pass through for the GUI to ignore. A
// missing log replays nothing. Reading starts at byte off, which must
// begin a line (a previous replay's end): the log only grows, so a
// reader that keeps up reads only what is new. Lines point into a copy
// of what was read in perm; scratch parses one line at a time.
static Replay logreplayat(Os *os, Arena *perm, Arena scratch, Str path,
                          Str sid, i64 from, i64 off, iz max)
{
    Replay r  = {};
    Read   rd = off ? os_readat(os, perm, path, off, max)
                    : os_read(os, perm, path, max);
    r.end = off;
    if (rd.err == READ_NOTFOUND) {
        r.ok = 1;
        return r;
    } else if (rd.err != READ_OK) {
        return r;
    }

    for (Str rest = rd.data;;) {
        Cut c = cut(rest, '\n');
        if (!c.ok) {
            break;  // end, or a torn tail
        }
        rest   = c.tail;
        r.end += c.head.len + 1;

        Arena tmp = scratch;
        EvRec rec = evparse(&tmp, c.head);
        if (!evvalid(&rec, sid, r.last)) {
            continue;
        }
        r.last = rec.seq;
        if (rec.seq > from) {
            Str line = {c.head.data, c.head.len+1};
            r.lines = push(perm, r.lines, line);
        }
    }
    r.ok = 1;
    return r;
}

static Replay logreplay(Os *os, Arena *perm, Arena scratch, Str path,
                        Str sid, i64 from, iz max)
{
    return logreplayat(os, perm, scratch, path, sid, from, 0, max);
}


// Copies of logs
//
// A fork is a new session made from another's log (logcopy): its records
// up to a cut, under the new sid, with their seq numbers, to which
// compaction records refer. The cut follows the end of a turn,
// turn.end{turn:N}, and keeps what came between turns after it (a job's
// end, a rewind, a model switch, notes) up to the next turn's first
// record, user or turn. Turn 0 keeps the settings with no history, and
// no turn the whole log. Every line kept names the new session: its sid,
// and its directory wherever a path named the old one, as a tool result
// names a spill file, in each spelling as JSON escapes it (a Windows
// path's backslashes doubled), so that the copy never depends on the
// original. The session record says where it came from (forked_from,
// fork_turn) right after ev, where the sessions list, which reads only a
// log's first 4 KiB, finds it, and a title record "TITLE (fork)" ends the
// log, after a mode record back to Build (by "fork") if the cut left it
// in Bypass mode. Background jobs still running at the cut go on in the
// original alone: the copy ends each with a job.end of status "forked"
// and no notice (notice:0), so that the fork's agent never hears of
// their end. It is pure, so that an import can make use of it too: an import
// (logimport) is a copy of the whole log, whose session record says
// imported_from instead, and whose title is "TITLE (imported)".
//
// A side chat (COPY_SIDE) is a fork of the whole log while the original
// may be at work: its session record says side_of too, and the copy
// closes the turn it cuts into, as a running engine would have: a
// result for every call without one, still running there (tool.end) or
// not yet run (an answer to its pending ask, tool.start and tool.end),
// each of status "cancelled" and saying so, then turn.end{why:"side"}.
// Records not yet committed there (a reply still streaming) are not in
// the log, and so not in the copy. A notice of kind "side" tells its
// agent what it is, just before the title "TITLE (side chat)".

enum : i32 {
    COPY_FORK,
    COPY_IMPORT,
    COPY_SIDE,
};

struct LogCopy {
    Str        log;    // the new events.jsonl, null on failure
    Str        err;    // why not
    Str        next;   // the prompt that began the turn after the cut
    Str        title;  // the copy's own
    Str        model;  // as a model record left it at the cut, else null
    i64        turn;   // the last turn kept
    b32        open;   // the lines kept end inside a turn
    Slice<Str> files;  // the out/, snap/ and img/ files they name, as "out/NAME"
};

// A string as JSON escapes it, without its quotes.
static Str jsonescaped(Arena *a, Str s)
{
    Buf b(a, s.len + 16);
    printjsonstr(&b, s);
    return cuttail(cuthead(finish(&b), 1), 1);
}

// Print s with each whole occurrence of olds[k], one not followed by more
// of a name, replaced by news[k].
static void printreplaced(Buf *b, Str s, Slice<Str> olds, Slice<Str> news)
{
    iz last = 0;
    for (iz i = 0; i < s.len;) {
        iz k = 0;
        for (; k < olds.len; k++) {
            Str o   = olds[k];
            iz  end = i + o.len;
            if (o.len && o.len<=s.len-i && s[i]==o[0] &&
                    !__builtin_memcmp(s.data+i, o.data, (uz)o.len) &&
                    (end==s.len || !(alnum(s[end]) || s[end]=='-' || s[end]=='_'))) {
                break;
            }
        }
        if (k == olds.len) {
            i++;
            continue;
        }
        print(b, span(s.data+last, s.data+i));
        print(b, news[k]);
        i   += olds[k].len;
        last = i;
    }
    print(b, cuthead(s, last));
}

// Whether a byte ends a file name written in a log.
static b32 nameend(u8 c)
{
    return c<0x20 || c=='"' || c=='\\' || c=='/' || c==' ' || c==',' || c==';' ||
           c==':' || c=='\'' || c=='`' || c=='(' || c==')' || c=='<' || c=='>' ||
           c=='[' || c==']' || c=='{' || c=='}' || c=='|' || c=='*' || c=='?';
}

// The out/, snap/ and img/ files a log names, as "out/NAME": in a relative
// path or after the session directory, with either separator, and also
// without the dots that may end a sentence. Some may name nothing.
static Slice<Str> lognames(Arena *a, Str log)
{
    static Str const dirs[] = {"out/", "snap/", "img/", "out\\\\", "snap\\\\"};
    Slice<Str> r    = {};
    Map<b32>  *seen = 0;
    for (iz i = 0; i < log.len; i++) {
        if (log[i]!='o' && log[i]!='s' && log[i]!='i') {
            continue;
        }
        for (iz k = 0; k < countof(dirs); k++) {
            Str d = dirs[k];
            if (d.len>log.len-i || __builtin_memcmp(log.data+i, d.data, (uz)d.len)) {
                continue;
            }
            iz beg = i + d.len;
            iz end = beg;
            for (; end<log.len && !nameend(log[end]); end++) {}
            Str name = span(log.data+beg, log.data+end);
            Str dir  = d[0]=='o' ? Str("out/") : d[0]=='i' ? Str("img/") : Str("snap/");
            for (i32 pass = 0; pass<2 && name.len; pass++) {
                Str  rel = concat(a, dir, name);
                b32 *got = upsert(&seen, rel, a);
                if (!*got) {
                    *got = 1;
                    r    = push(a, r, rel);
                }
                for (; name.len && name[name.len-1]=='.'; name.len--) {}
            }
            break;
        }
    }
    return r;
}

// The notice that begins a side chat of session from, titled title.
static Str sidenotice(Arena *a, Str from, Str title)
{
    Buf b(a, 256 + title.len);
    print(&b, "This is a side chat with the user, forked from the session ");
    if (title.len) {
        putbyte(&b, '"');
        print(&b, title);
        print(&b, "\" (");
        print(&b, from);
        putbyte(&b, ')');
    } else {
        print(&b, from);
    }
    print(&b, ", where the main agent may still be working. Do not change "
              "files unless the user asks you to.");
    return finish(&b);
}

// An id and the call it goes with, as raw JSON text, while copying.
struct CopyRef {
    Str id;
    Str call;
};

// Copy a log, its replayed lines, for session to, cut after turn (all of
// it if negative), with the session directory spelled olds[k] replaced
// by news[k] in every line, and a title record at now. With win, each in
// either separator too. An import's copy (COPY_IMPORT) says so instead,
// and a side chat's (COPY_SIDE) is whole and closes its open turn.
static LogCopy logcopy(Arena *a, Arena scratch, Slice<Str> lines, Str from,
                       Str to, Slice<Str> olds, Slice<Str> news, i64 turn,
                       i64 now, b32 win, i32 kind = COPY_FORK)
{
    b32 imported = kind == COPY_IMPORT;
    b32 side     = kind == COPY_SIDE;
    turn = side ? -1 : turn;
    LogCopy r = {};
    if (!lines.len || evparse(&scratch, lines[0]).ev!=EV_SESSION) {
        r.err = "the session's log does not begin with its session record";
        return r;
    } else if (jsonget(evparse(&scratch, lines[0]).root, "parent")->type == JSON_STRING) {
        r.err = "a subagent cannot be forked, only the session it worked for";
        return r;
    }

    // The cut: after turn.end{turn}, then on to the next turn's start
    // (end -2 while looking for it)
    iz  end   = turn<0 ? lines.len : -1;
    i64 most  = 0;
    Str title = {};
    for (iz i = 0; i < lines.len; i++) {
        Arena t     = scratch;
        EvRec rec   = evparse(&t, lines[i]);
        b32   begin = i>0 && (rec.ev==EV_USER || rec.ev==EV_TURN);
        if (rec.ev == EV_TITLE) {
            title = clone(a, jsonstr(jsonget(rec.root, "text")));
        } else if (rec.ev == EV_TURN) {
            i64 n = jsonint(jsonget(rec.root, "n"), 0);
            most  = n>most ? n : most;
        }
        if (end==-1 && turn==0 && begin) {
            end = i;
        } else if (end==-1 && turn>0 && rec.ev==EV_TURNEND &&
                   jsonint(jsonget(rec.root, "turn"), -1)==turn) {
            end = -2;
        } else if (end==-2 && begin) {
            end = i;
        }
    }
    end = end==-2 || (end==-1 && turn==0) ? lines.len : end;
    if (end < 0) {
        Buf b(&scratch, 64);
        print(&b, turn>most ? Str("no turn ") : Str("turn "));
        print(&b, turn);
        print(&b, turn>most ? Str(" in this session") : Str(" has not ended"));
        r.err = clone(a, finish(&b));
        return r;
    }

    // What the cut leaves, with its open calls, asks and jobs by the raw
    // text of their ids, which point into lines: each push follows the
    // last use of that line's parse, as in logopen
    b32            bypass   = 0;
    Str            planfile = {};
    Str            cwd      = {};  // raw, from the session record
    Slice<Str>     tools    = {};  // started, not ended
    Slice<Str>     ended    = {};  // since the last assistant record
    Slice<CopyRef> asks     = {};  // not answered: {req, call}
    Slice<CopyRef> jobs     = {};  // started, not ended: {job, call}
    iz             asst     = -1;  // the last assistant record
    for (iz i = 0; i < end; i++) {
        Arena t    = scratch;
        EvRec rec  = evparse(&t, lines[i]);
        Json *id   = jsonget(rec.root, "call");
        Str   call = id->type==JSON_STRING ? id->raw : Str{};
        Json *rq   = jsonget(rec.root, "req");
        Str   req  = rq->type==JSON_STRING ? rq->raw : Str{};
        switch (rec.ev) {
        case EV_SESSION: {
            Json *dir = jsonget(rec.root, "cwd");
            cwd = dir->type==JSON_STRING ? dir->raw : Str{};
        } break;
        case EV_ASSISTANT:
            asst      = i;
            ended.len = 0;
            break;
        case EV_TOOLSTART:
            if (call.data) {
                tools = push(&scratch, tools, call);
            }
            break;
        case EV_TOOLEND:
            logforget(&tools, call);
            if (call.data) {
                ended = push(&scratch, ended, call);
            }
            break;
        case EV_ASK:
            if (req.data && call.data) {
                asks = push(&scratch, asks, CopyRef{req, call});
            }
            break;
        case EV_ANSWER:
            for (iz k = 0; k < asks.len; k++) {
                if (asks[k].id == req) {
                    asks[k] = asks[asks.len-1];
                    asks.len--;
                    break;
                }
            }
            break;
        case EV_JOBSTART: {
            Json *n = jsonget(rec.root, "job");
            if (n->type==JSON_NUMBER && call.data) {
                jobs = push(&scratch, jobs, CopyRef{n->raw, call});
            }
        } break;
        case EV_JOBEND: {
            Str n = jsonget(rec.root, "job")->raw;
            for (iz k = 0; k < jobs.len; k++) {
                if (jobs[k].id == n) {
                    for (iz m = k+1; m < jobs.len; m++) {
                        jobs[m-1] = jobs[m];
                    }
                    jobs.len--;
                    break;
                }
            }
        } break;
        case EV_TURN:
            r.turn = jsonint(jsonget(rec.root, "n"), r.turn);
            r.open = 1;
            break;
        case EV_TURNEND:
            r.open = 0;
            break;
        case EV_MODEL:
            r.model = clone(a, jsonstr(jsonget(rec.root, "model")));
            break;
        case EV_MODE:
            bypass   = jsonstr(jsonget(rec.root, "mode")) == "bypass";
            planfile = clone(a, jsonstr(jsonget(rec.root, "path")));
            break;
        }
    }

    // In a side chat's open turn, the calls of its last assistant record
    // that never started
    Slice<Json *> unstarted = {};
    if (side && r.open && asst>=0) {
        EvRec rec = evparse(&scratch, lines[asst]);
        for (Json *k = jsonget(rec.root, "blocks")->child; k; k = k->next) {
            Json *cid  = jsonget(k, "id");
            Str   call = cid->type==JSON_STRING ? cid->raw : Str{};
            if (jsonstr(jsonget(k, "t"))!="call" || !call.data) {
                continue;
            }
            b32 seen = 0;
            for (iz i = 0; i<tools.len && !seen; i++) {
                seen = tools[i] == call;
            }
            for (iz i = 0; i<ended.len && !seen; i++) {
                seen = ended[i] == call;
            }
            if (!seen) {
                unstarted = push(&scratch, unstarted, k);
            }
        }
    }
    if (end < lines.len) {
        EvRec rec = evparse(&scratch, lines[end]);
        if (rec.ev == EV_USER) {
            r.next = clone(a, jsonstr(jsonget(rec.root, "text")));
        }
    }
    Str suffix = imported ? Str(" (imported)") :
                 side     ? Str(" (side chat)") : Str(" (fork)");
    r.title = concat(a, title.len ? title : Str("Untitled"), suffix);
    r.turn  = turn<0 ? r.turn : turn;

    // Every spelling of the directory, escaped as in a line
    Slice<Str> o = {};
    Slice<Str> n = {};
    for (iz k = 0; k<olds.len && k<news.len; k++) {
        for (i32 form = 0; form < (win ? 3 : 1); form++) {
            Str x = clone(&scratch, olds[k]);
            Str y = clone(&scratch, news[k]);
            u8  c = form==1 ? '/' : '\\';
            for (iz i = 0; form && i<x.len; i++) {
                x[i] = x[i]=='/' || x[i]=='\\' ? c : x[i];
            }
            for (iz i = 0; form && i<y.len; i++) {
                y[i] = y[i]=='/' || y[i]=='\\' ? c : y[i];
            }
            x = jsonescaped(&scratch, x);
            b32 dup = !x.len;
            for (iz i = 0; i<o.len && !dup; i++) {
                dup = o[i] == x;
            }
            if (!dup) {
                o = push(&scratch, o, x);
                n = push(&scratch, n, jsonescaped(&scratch, y));
            }
        }
    }

    Str head    = concat(&scratch, "{\"sid\":\"", jsonescaped(&scratch, from));
    head        = concat(&scratch, head, "\",");
    Str newhead = concat(&scratch, "{\"sid\":\"", jsonescaped(&scratch, to));
    newhead     = concat(&scratch, newhead, "\",");
    i64 last    = 0;
    Buf out(a, 4096);
    for (iz i = 0; i < end; i++) {
        Arena t    = scratch;
        Str   line = lines[i];
        EvRec rec  = evparse(&t, line);
        last = rec.seq;
        if (i == 0) {
            // Where it came from, early in the session record
            Buf   b(&t, line.len + 256);
            EvOut w = evbegin(&b, to, rec.seq, rec.ts, EV_SESSION);
            if (imported) {
                putstrkey(&w.json, "imported_from", from);
            } else {
                putstrkey(&w.json, "forked_from", from);
                putintkey(&w.json, "fork_turn", r.turn);
                if (side) {
                    putstrkey(&w.json, "side_of", from);
                }
            }
            for (Json *m = rec.root->child; m; m = m->next) {
                if (m->key=="sid" || m->key=="seq" || m->key=="ts" || m->key=="ev" ||
                        m->key=="forked_from" || m->key=="fork_turn" ||
                        m->key=="side_of") {
                    continue;
                }
                putkey(&w.json, m->key);
                putjson(&w.json, m);
            }
            line = evend(&w);
        } else if (startswith(line, head)) {
            line = concat(&t, newhead, cuthead(line, head.len));
        } else {
            // Written some other way: the record anew, with the new sid
            Buf     b(&t, line.len + 256);
            JsonOut w = {&b};
            putobject(&w);
            putstrkey(&w, "sid", to);
            for (Json *m = rec.root->child; m; m = m->next) {
                if (!(m->key == "sid")) {
                    putkey(&w, m->key);
                    putjson(&w, m);
                }
            }
            putend(&w);
            putbyte(&b, '\n');
            line = finish(&b);
        }
        printreplaced(&out, line, o, n);
    }

    // A side chat closes the turn it cut into
    for (iz i = 0; side && r.open && i<tools.len; i++) {
        Buf   b(&scratch, 256);
        EvOut w = evbegin(&b, to, ++last, now, EV_TOOLEND);
        putkey(&w.json, "call");
        putraw(&w.json, tools[i]);
        putstrkey(&w.json, "status", "cancelled");
        putstrkey(&w.json, "text", "[still running in the main session when "
                                   "this side chat began]");
        printreplaced(&out, evend(&w), o, n);
    }
    for (iz i = 0; side && r.open && i<unstarted.len; i++) {
        Json *k    = unstarted[i];
        Str   call = jsonget(k, "id")->raw;
        for (iz q = 0; q < asks.len; q++) {
            if (!(asks[q].call == call)) {
                continue;
            }
            Buf   b(&scratch, 256);
            EvOut w = evbegin(&b, to, ++last, now, EV_ANSWER);
            putkey(&w.json, "req");
            putraw(&w.json, asks[q].id);
            putstrkey(&w.json, "decision", "deny");
            putstrkey(&w.json, "by", "cancel");
            putstrkey(&w.json, "note", "asked in the main session when this "
                                       "side chat began");
            printreplaced(&out, evend(&w), o, n);
        }
        Str   args = jsonstr(jsonget(k, "args"));
        Buf   b(&scratch, 256 + args.len + args.len/8 + cwd.len);
        EvOut w = evbegin(&b, to, ++last, now, EV_TOOLSTART);
        putkey(&w.json, "call");
        putraw(&w.json, call);
        putstrkey(&w.json, "name", jsonstr(jsonget(k, "name")));
        putstrkey(&w.json, "args", args);
        putkey(&w.json, "cwd");
        if (cwd.data) {
            putraw(&w.json, cwd);
        } else {
            putstr(&w.json, "");
        }
        printreplaced(&out, evend(&w), o, n);

        b.len = 0;
        w = evbegin(&b, to, ++last, now, EV_TOOLEND);
        putkey(&w.json, "call");
        putraw(&w.json, call);
        putstrkey(&w.json, "status", "cancelled");
        putstrkey(&w.json, "text", "[not yet run in the main session when "
                                   "this side chat began]");
        printreplaced(&out, evend(&w), o, n);
    }
    for (iz i = 0; !imported && i<jobs.len; i++) {
        // It goes on in the original alone, with nothing to tell here
        Buf   b(&scratch, 256);
        EvOut w = evbegin(&b, to, ++last, now, EV_JOBEND);
        putkey(&w.json, "job");
        putraw(&w.json, jobs[i].id);
        putkey(&w.json, "call");
        putraw(&w.json, jobs[i].call);
        putstrkey(&w.json, "status", "forked");
        putstrkey(&w.json, "text", side ? Str("still running in the main session "
                                              "when this side chat began")
                                        : Str("still running in the original "
                                              "session when this fork was made"));
        putintkey(&w.json, "notice", 0);
        printreplaced(&out, evend(&w), o, n);
    }
    if (side && r.open) {
        Buf   b(&scratch, 128);
        EvOut w = evbegin(&b, to, ++last, now, EV_TURNEND);
        putintkey(&w.json, "turn", r.turn);
        putstrkey(&w.json, "why", "side");
        printreplaced(&out, evend(&w), o, n);
        r.open = 0;
    }
    if (bypass && !imported) {
        // Bypass was the user's pick for that session alone
        Buf   b(&scratch, 256 + planfile.len);
        EvOut w = evbegin(&b, to, ++last, now, EV_MODE);
        putstrkey(&w.json, "mode", "default");
        putstrkey(&w.json, "by", "fork");
        putstrkey(&w.json, "path", planfile);
        printreplaced(&out, evend(&w), o, n);
    }
    if (side) {
        // What it is, durable, so that its first request has it
        Str   text = sidenotice(&scratch, from, title);
        Buf   b(&scratch, 256 + text.len + text.len/8);
        EvOut w = evbegin(&b, to, ++last, now, EV_NOTICE);
        putstrkey(&w.json, "kind", "side");
        putstrkey(&w.json, "text", text);
        print(&out, evend(&w));
    }
    {
        Buf   b(&scratch, 256 + r.title.len);
        EvOut w = evbegin(&b, to, last+1, now, EV_TITLE);
        putstrkey(&w.json, "text", r.title);
        putstrkey(&w.json, "by", "auto");
        print(&out, evend(&w));
    }
    r.log   = finish(&out);
    r.files = lognames(a, r.log);
    return r;
}


// Imports
//
// An import is a new session from a raw log written elsewhere, perhaps
// on another machine, and so not trusted: its records may name any
// path, and some would have the engine read or write there (a snap's
// path, which rewind writes; a tool result's image, read into requests)
// or widen the guard (roots, temp, grants). Only the conversation is
// kept, by an allowlist of events, and of those records the fields
// that name files of the old session's directory are dropped, since
// those files never come along. The session record is made anew for
// where the session now works (cwd, model, endpoint, scratch), keeping
// the role, its prompt and tools, the edit format and the step limit.
// With the prefix and instruction records gone, opening the session
// builds its prefix afresh, for its new place. Foreign paths in the
// text of old calls and results stay, as history. A torn final line is
// left out, and a log whose lines are mostly not records of its session
// is refused. The copy is then logcopy's, imported, whole.

struct LogImport {
    Str log;      // the new events.jsonl, null on failure
    Str err;      // why not
    Str from;     // the session it was
    Str title;    // the import's own
    i32 kept;     // records kept
    i32 dropped;  // records of events not kept
    i32 bad;      // lines that were no record of the session
};

// Whether an import keeps records of an event: the conversation's.
static b32 importkeeps(i32 ev)
{
    switch (ev) {
    case EV_TITLE:
    case EV_USER:
    case EV_TURN:
    case EV_ATTEMPT:
    case EV_ASSISTANT:
    case EV_TOOLSTART:
    case EV_TOOLEND:
    case EV_RETRY:
    case EV_NUDGE:
    case EV_TURNEND:
    case EV_NOTE:
    case EV_COMPACT:
    case EV_TODO:
    case EV_NOTICE:
        return 1;
    }
    return 0;
}

// Whether an import drops a field: of a tool result, one naming a file
// or a session of the old state directory (spill, image, child, a
// message's target), or describing one; of a message's notice, its
// sender, whose title and text stay; of a user record, its images.
static b32 importdrops(i32 ev, Str key)
{
    if (ev == EV_NOTICE) {
        return key == "from";
    } else if (ev == EV_USER) {
        return key == "images";
    }
    return key=="spill" || key=="bytes" || key=="image" || key=="width" ||
           key=="height" || key=="shot" || key=="child" || key=="job" ||
           key=="to";
}

// A tool result, notice or user record without the fields an import
// drops, or the line itself if it has none.
static Str importstrip(Arena *a, EvRec *rec, Str line)
{
    b32 any = 0;
    for (Json *m = rec->root->child; m; m = m->next) {
        any |= importdrops(rec->ev, m->key);
    }
    if (!any) {
        return line;
    }
    Buf     b(a, line.len);
    JsonOut w = {&b};
    putobject(&w);
    for (Json *m = rec->root->child; m; m = m->next) {
        if (!importdrops(rec->ev, m->key)) {
            putkey(&w, m->key);
            putjson(&w, m);
        }
    }
    putend(&w);
    putbyte(&b, '\n');
    return finish(&b);
}

// The session record made anew for where the session now works: in a
// folder of its own with nofolder (new {nofolder}).
static Str importsession(Arena *a, EvRec *rec, Str cwd, Str model,
                         Str endpoint, Str scratch, b32 nofolder)
{
    static Str const kept[] = {"role", "prompt", "tools", "edit_format", "steps"};
    Json *j = rec->root;
    Buf   b(a, 1024 + cwd.len + model.len + endpoint.len + scratch.len);
    EvOut w = evbegin(&b, rec->sid, rec->seq, rec->ts, EV_SESSION);
    putintkey(&w.json, "proto", 0);
    Json *v = jsonget(j, "version");
    if (v->type == JSON_STRING) {
        putstrkey(&w.json, "version", jsonstr(v));
    }
    putstrkey(&w.json, "cwd", cwd);
    putstrkey(&w.json, "model", model);
    putstrkey(&w.json, "endpoint", endpoint);
    if (scratch.len) {
        putstrkey(&w.json, "scratch", scratch);
    }
    for (iz k = 0; k < countof(kept); k++) {
        Json *m = jsonget(j, kept[k]);
        if (m->type != JSON_MISSING) {
            putkey(&w.json, kept[k]);
            putjson(&w.json, m);
        }
    }
    if (nofolder) {
        putintkey(&w.json, "nofolder", 1);
    }
    return evend(&w);
}

// An import of a raw log, data, as session to, working in cwd with the
// model at the endpoint (a URL), with its scratch directory there, and
// titled at now, in a folder of its own with nofolder (cwd). A session
// is listed only by a first line under 4 KiB.
static LogImport logimport(Arena *a, Arena scratch, Str data, Str to, Str cwd,
                           Str model, Str endpoint, Str dir, i64 now,
                           b32 nofolder = 0)
{
    LogImport r = {};

    // Its complete lines, a torn tail left out
    Slice<Str> lines = {};
    for (Str rest = data;;) {
        Cut c = cut(rest, '\n');
        if (!c.ok) {
            break;
        }
        rest  = c.tail;
        lines = push(&scratch, lines, Str{c.head.data, c.head.len+1});
    }

    {
        Arena t   = scratch;
        EvRec rec = lines.len ? evparse(&t, lines[0]) : EvRec{};
        if (!rec.ok || rec.ev!=EV_SESSION || rec.seq!=1 || !validsid(rec.sid)) {
            r.err = "not a session's raw log: it must begin with its session record";
            return r;
        } else if (jsonget(rec.root, "parent")->type == JSON_STRING) {
            r.err = "a subagent's log cannot be imported, only its parent's";
            return r;
        }
        r.from = clone(a, rec.sid);
    }

    Slice<Str> kept = {};
    i64        last = 0;
    for (iz i = 0; i < lines.len; i++) {
        Arena t    = scratch;
        Str   line = lines[i];
        EvRec rec  = evparse(&t, line);
        if (!evvalid(&rec, r.from, last)) {
            r.bad++;
            continue;
        }
        last = rec.seq;
        if (i == 0) {
            line = importsession(a, &rec, cwd, model, endpoint, dir, nofolder);
        } else if (!importkeeps(rec.ev)) {
            r.dropped++;
            continue;
        } else if (rec.ev==EV_TOOLEND || rec.ev==EV_NOTICE || rec.ev==EV_USER) {
            line = importstrip(a, &rec, line);
        }
        kept = push(&scratch, kept, line);  // t is dead now
        r.kept++;
    }
    if (r.bad > lines.len/2) {
        r.err = "most of its lines are not records of its session";
        return r;
    }

    LogCopy c = logcopy(a, scratch, kept, r.from, to, {}, {}, -1, now, 0, COPY_IMPORT);
    if (c.err.len) {
        r.err = c.err;
        return r;
    } else if (cut(c.log, '\n').head.len >= 4096) {
        r.err = "its session record is too long to be listed: over 4 KiB";
        return r;
    }
    r.log   = c.log;
    r.title = c.title;
    return r;
}
