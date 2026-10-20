// Protocol v0 records and session event logs
//
// A record is one JSON object on one line ending in \n, with keys in a
// fixed order: sid, seq (durable records only), ts, ev, then the payload.
// Commands from the GUI start with cmd and id instead. A session's log,
// events.jsonl, holds exactly its durable records, byte for byte as they
// are sent to the GUI, so replaying a session means streaming its file.
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
    {"delta",      0, 0},
    {"progress",   0, 0},
    {"tool.out",   0, 0},
    {"live",       0, 0},
    {"snapshot",   0, 0},
    {"sessions",   0, 0},
    {"overflow",   0, 0},
    {"hello",      0, 0},
    {"reply",      0, 0},
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
    CMD_COUNT,
};

static Str cmdnames[] = {
    "", "hello", "list", "new", "open", "close", "prompt", "steer",
    "cancel", "answer", "config.get", "config.set", "quit",
};
static_assert(countof(cmdnames) == CMD_COUNT);

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

// Open a session log for appending, creating it if missing. A torn final
// line is truncated away, or ended with a newline if the log cannot be
// truncated. Lines that are not records of this session in increasing
// seq order are skipped (and counted). Then every tool.start without a
// tool.end gets tool.end{status:"unknown"}, and an open turn gets
// turn.end{why:"interrupted"}. Reading needs max bytes of scratch at
// most (a larger log fails), plus parsing space for one line.
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

    // Track open tools by the raw text of their call ids (the writer's
    // escaping is canonical), which points into data and so outlives the
    // parse of each line.
    Slice<Str> tools    = {};
    b32        turnopen = 0;
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
        switch (rec.ev) {
        case EV_TURN:
            r.turn   = jsonint(jsonget(rec.root, "n"), r.turn);
            turnopen = 1;
            break;
        case EV_TURNEND:
            turnopen = 0;
            break;
        case EV_TOOLSTART:
            if (call.data) {
                tools = push(&scratch, tools, call);  // tmp is dead now
            }
            break;
        case EV_TOOLEND:
            for (iz i = 0; i < tools.len; i++) {
                if (tools[i] == call) {
                    for (iz j = i+1; j < tools.len; j++) {
                        tools[j-1] = tools[j];
                    }
                    tools.len--;
                    break;
                }
            }
            break;
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
    b32        ok;
};

// Read the durable records with seq > from, as logopen would accept
// them, without modifying the log: a torn tail and invalid lines are
// skipped, while unknown events pass through for the GUI to ignore. A
// missing log replays nothing. Lines point into a copy of the file in
// perm; scratch parses one line at a time.
static Replay logreplay(Os *os, Arena *perm, Arena scratch, Str path,
                        Str sid, i64 from, iz max)
{
    Replay r  = {};
    Read   rd = os_read(os, perm, path, max);
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
        rest = c.tail;

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
