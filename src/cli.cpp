// Command line: prospero run, and prospero --headless
//
//   prospero run [options] PROMPT        one turn, printing protocol records
//   prospero run [options] --resume SID [PROMPT]
//   prospero --headless [options]        protocol v0 on standard I/O
//
// Both drive the engine thread exactly as the GUI will: commands in,
// records out. "run" prints each record as a JSONL line, or readable
// text with --pretty, and exits when the turn ends: 0 when done, 1 on
// error, 3 at the step budget, 130 when cancelled, 2 for usage errors
// (including a session open read-only, in use by another process).
// While background jobs run after a turn that is done, it waits for
// them, through the turns the engine starts by itself to report them,
// and exits with the last turn's code once no job runs and no turn
// follows; quitting kills any job left.
// Nobody answers asks in a run, so they are denied by policy. Over
// --headless, a frontend that answers them opens sessions attended
// (attended:1 on new and open), as the GUI's --engine=subprocess will;
// when its input ends, the engine stops as for quit. A run prints the
// records of its subagents' sessions too, as JSONL, but --pretty shows
// only its own session's.
//
// This is free and unencumbered software released into the public domain.

static Str cliusage =
"usage: prospero run [options] PROMPT\n"
"       prospero run [options] --resume SID [PROMPT]\n"
"       prospero --headless [options]\n"
"options:\n"
"  -C DIR        working directory (default: the current one)\n"
"  -e ENDPOINT   endpoint URL or configured name\n"
"  -m MODEL      model name\n"
"  -r ROLE       role, a [role] in the configuration (run only)\n"
"  -c FILE       configuration file\n"
"  --state DIR   session storage directory\n"
"  --pretty      readable output instead of JSONL (run only)\n";

struct Cli {
    Os      *os;
    Engine  *e;
    Mailbox *out;
    Str      statedir;
    Str      sid;
    Str      model;     // the session's own, for --pretty
    b32      pretty;
    b32      headless;
    b32      quitting;
    i64      lastseq;   // the last durable record printed
    i32      exitcode;
    i32      jobs;      // background jobs running
    b32      turning;   // a turn runs, or one follows
    i64      openid;
    Str      prompt;    // for --resume
    i32      block;     // the delta block printed last, -1 if none
    b32      midline;   // pretty output does not end in a newline
    Arena    keep;      // for the session id and model
    Region  *spare;     // log copies for resyncs
};

static void clicmd(Cli *c, Arena scratch, i32 cmd, i64 id, Str sid, Str key, Str val)
{
    Buf   b(&scratch, 512 + val.len);
    EvOut w = cmdbegin(&b, cmd, id);
    if (sid.len) {
        putkey(&w.json, "sid");
        putstr(&w.json, sid);
    }
    if (key.len) {
        putkey(&w.json, key);
        putstr(&w.json, val);
    }
    enginecmd(c->e, cuttail(evend(&w), 1));
}

static void cliquit(Cli *c, Arena scratch)
{
    if (!c->quitting) {
        c->quitting = 1;
        clicmd(c, scratch, CMD_QUIT, 99, {}, {}, {});
    }
}

static void prettyline(Cli *c)
{
    if (c->midline) {
        os_print(c->os, 1, "\n");
        c->midline = 0;
    }
}

// Indented excerpt of a tool result.
static void prettyexcerpt(Cli *c, Arena scratch, Str text)
{
    Buf b(&scratch, 1024);
    iz  n = 0;
    for (Str rest = text; rest.len; n++) {
        Cut l = cut(rest, '\n');
        rest = l.tail;
        if (n < 12) {
            print(&b, "  ");
            print(&b, l.head.len>200 ? utf8head(l.head, 200) : l.head);
            putbyte(&b, '\n');
        }
    }
    if (n > 12) {
        print(&b, "  ... (");
        print(&b, n - 12);
        print(&b, " more lines)\n");
    }
    os_print(c->os, 1, finish(&b));
}

// A wait for people: "750 ms", "45 s", "15 min".
static void prettywait(Buf *b, i64 ms)
{
    if (ms < 10000) {
        print(b, ms);
        print(b, " ms");
    } else if (ms < 120000) {
        print(b, (ms + 500)/1000);
        print(b, " s");
    } else {
        print(b, (ms + 30000)/60000);
        print(b, " min");
    }
}

static void prettyrecord(Cli *c, Arena scratch, EvRec *r)
{
    Os   *os = c->os;
    Json *j  = r->root;
    Buf   b(&scratch, 1024);
    switch (r->ev) {
    case EV_SESSION:
        c->model = clone(&c->keep, jsonstr(jsonget(j, "model")));
        return;
    case EV_USER: {
        // A skill invoked shows as typed, its instructions left out
        prettyline(c);
        Str skill = jsonstr(jsonget(j, "skill"));
        Str args  = jsonstr(jsonget(j, "args"));
        print(&b, "> ");
        if (skill.len) {
            putbyte(&b, '/');
            print(&b, skill);
            print(&b, args.len ? Str(" ") : Str(""));
            print(&b, args);
        } else {
            print(&b, jsonstr(jsonget(j, "text")));
        }
        print(&b, "\n");
    } break;
    case EV_DELTA: {
        Str kind  = jsonstr(jsonget(j, "kind"));
        i32 block = (i32)jsonint(jsonget(j, "block"), 0);
        if (kind == "args") {
            return;
        }
        if (block != c->block) {
            prettyline(c);
            if (kind == "think") print(&b, "[thinking] ");
            c->block = block;
        }
        print(&b, jsonstr(jsonget(j, "text")));
        c->midline = 1;
    } break;
    case EV_ASSISTANT:
        c->block = -1;
        return;
    case EV_TOOLSTART: {
        prettyline(c);
        Str     name = jsonstr(jsonget(j, "name"));
        Repair  rp   = repairargs(&scratch, jsonstr(jsonget(j, "args")));
        Str     what = name=="shell" ? jsonstr(jsonget(rp.root, "command")) :
                       name=="task"  ? jsonstr(jsonget(rp.root, "description")) :
                       name=="skill" ? jsonstr(jsonget(rp.root, "name")) :
                       name=="send_message" ? jsonstr(jsonget(rp.root, "to")) :
                       jsonstr(jsonget(rp.root, "path"));
        Json   *pt   = jsonget(rp.root, "patch");
        if (name=="edit" && pt->type==JSON_STRING) {
            // A patch names its files inside
            Slice<Str> paths = patchpaths(&scratch, pt->str);
            Buf        p(&scratch, 256);
            for (iz i = 0; i < paths.len; i++) {
                print(&p, i ? Str(", ") : Str(""));
                print(&p, paths[i]);
            }
            what = finish(&p);
        }
        print(&b, "$ ");
        print(&b, name);
        print(&b, ": ");
        print(&b, what.len ? what : jsonstr(jsonget(j, "args")));
        print(&b, "\n");
    } break;
    case EV_TOOLEND:
        prettyline(c);
        os_print(os, 1, concat(&scratch, concat(&scratch, "  [",
                 jsonstr(jsonget(j, "status"))), "]\n"));
        prettyexcerpt(c, scratch, jsonstr(jsonget(j, "text")));
        return;
    case EV_RETRY: {
        // A retry names its target, the next attempt's: a switch to a
        // fallback, or a wait for the same model or the primary
        prettyline(c);
        Str model = jsonstr(jsonget(j, "model"));
        i64 fall  = jsonint(jsonget(j, "fallback"), 0);
        b32 slow  = jsonflag(jsonget(j, "slow"), 0);
        print(&b, jsonflag(jsonget(j, "compact"), 0) ? Str("[compaction: ")
                                                      : Str("["));
        if (fall) {
            print(&b, "switching to fallback ");
            print(&b, fall);
            print(&b, ", ");
            print(&b, model);
        } else {
            print(&b, slow ? Str("slow retry in ") : Str("retry in "));
            prettywait(&b, jsonint(jsonget(j, "delay_ms"), 0));
            if (model.len && (slow || model!=c->model)) {
                print(&b, " on ");
                print(&b, model);
            }
        }
        print(&b, ": ");
        print(&b, jsonstr(jsonget(j, "msg")));
        print(&b, "]\n");
    } break;
    case EV_COMPACT: {
        prettyline(c);
        print(&b, "[compacted: ");
        if (jsonstr(jsonget(j, "kind")) == "summary") {
            print(&b, "older messages summarized by ");
            print(&b, jsonstr(jsonget(j, "model")));
        } else {
            i64 n = jsonlen(jsonget(j, "elided"));
            print(&b, n);
            print(&b, n==1 ? Str(" old tool output elided")
                           : Str(" old tool outputs elided"));
        }
        print(&b, ", about ");
        print(&b, jsonint(jsonget(j, "before"), 0));
        print(&b, " -> ");
        print(&b, jsonint(jsonget(j, "after"), 0));
        print(&b, " tokens");
        if (jsonstr(jsonget(j, "why")) == "overflow") {
            print(&b, ", since the server found the prompt too long");
        }
        print(&b, "]\n");
    } break;
    case EV_NUDGE:
        prettyline(c);
        print(&b, "[nudge: ");
        print(&b, jsonstr(jsonget(j, "kind")));
        print(&b, "]\n");
        break;
    case EV_NOTE:
        prettyline(c);
        print(&b, "[note: ");
        print(&b, jsonstr(jsonget(j, "text")));
        print(&b, "]\n");
        break;
    case EV_ASK:
        prettyline(c);
        if (jsonstr(jsonget(j, "kind")) == "plan") {
            os_print(os, 1, "[asking to approve the plan:]\n");
            prettyexcerpt(c, scratch, jsonstr(jsonget(j, "detail")));
            return;
        }
        print(&b, "[asking: ");
        print(&b, jsonstr(jsonget(j, "detail")));
        print(&b, "]\n");
        break;
    case EV_MODE:
        prettyline(c);
        print(&b, jsonstr(jsonget(j, "mode"))=="plan" ? Str("[plan mode on, by ")
                                                      : Str("[plan mode off, by "));
        print(&b, jsonstr(jsonget(j, "by")));
        print(&b, "]\n");
        break;
    case EV_ANSWER: {
        prettyline(c);
        Str note = jsonstr(jsonget(j, "note"));
        print(&b, jsonstr(jsonget(j, "decision"))=="allow" ? Str("[allowed by ")
                                                           : Str("[denied by "));
        print(&b, jsonstr(jsonget(j, "by")));
        if (note.len) {
            print(&b, ": ");
            print(&b, note);
        }
        print(&b, "]\n");
    } break;
    case EV_TURNEND: {
        prettyline(c);
        Str err = jsonstr(jsonget(j, "err"));
        print(&b, "[turn ");
        print(&b, jsonint(jsonget(j, "turn"), 0));
        print(&b, " ended: ");
        print(&b, jsonstr(jsonget(j, "why")));
        if (err.len) {
            print(&b, ": ");
            print(&b, err);
        }
        print(&b, "]\n");
    } break;
    case EV_JOBSTART:
        prettyline(c);
        print(&b, "[job ");
        print(&b, jsonint(jsonget(j, "job"), 0));
        print(&b, " started: ");
        print(&b, jsonstr(jsonget(j, "command")));
        print(&b, "]\n");
        break;
    case EV_JOBEND: {
        prettyline(c);
        Json *code = jsonget(j, "exit");
        print(&b, "[job ");
        print(&b, jsonint(jsonget(j, "job"), 0));
        print(&b, " ended: ");
        print(&b, jsonstr(jsonget(j, "status")));
        if (code->type == JSON_NUMBER) {
            print(&b, ", exit code ");
            print(&b, jsonint(code, 0));
        }
        print(&b, "]\n");
    } break;
    case EV_TODO: {
        prettyline(c);
        i64 n = 0, done = 0;
        for (Json *k = jsonget(j, "items")->child; k; k = k->next, n++) {
            done += jsonstr(jsonget(k, "status")) == "done";
        }
        print(&b, "[todo: ");
        print(&b, done);
        print(&b, " of ");
        print(&b, n);
        print(&b, " done]\n");
    } break;
    case EV_NOTICE: {
        // A message from another session names its sender
        prettyline(c);
        Str kind  = jsonstr(jsonget(j, "kind"));
        Str title = jsonstr(jsonget(j, "from_title"));
        Str from  = title.len ? title : jsonstr(jsonget(j, "from"));
        if (kind=="message" && from.len) {
            print(&b, "[message from ");
            print(&b, from);
        } else {
            print(&b, "[notice: ");
            print(&b, kind);
        }
        print(&b, "]\n");
        os_print(os, 1, finish(&b));
        prettyexcerpt(c, scratch, jsonstr(jsonget(j, "text")));
    } return;
    case EV_REWIND:
        prettyline(c);
        print(&b, "[rewound ");
        print(&b, jsonlen(jsonget(j, "files")));
        print(&b, " files]\n");
        break;
    case EV_INSTRUCTIONS:
        if (!jsonlen(jsonget(j, "files"))) {
            return;  // none were found
        }
        prettyline(c);
        print(&b, "[instructions:");
        for (Json *k = jsonget(j, "files")->child; k; k = k->next) {
            putbyte(&b, ' ');
            print(&b, jsonstr(jsonget(k, "path")));
        }
        print(&b, "]\n");
        break;
    default:
        return;
    }
    os_print(os, 1, finish(&b));
}

static i32 exitcodefor(Str why)
{
    if (why == "done")   return 0;
    if (why == "cancel") return 130;
    if (why == "budget") return 3;
    return 1;
}

// Print one record and follow the run.
static void clirecord(Cli *c, Arena scratch, Str line)
{
    Arena t = scratch;
    EvRec r = evparse(&t, line);
    if (!c->headless && r.seq && r.sid==c->sid) {
        if (r.seq <= c->lastseq) {
            return;  // already printed during a resync
        }
        c->lastseq = r.seq;
    }
    if (!c->pretty) {
        os_print(c->os, 1, line);
    } else if (c->headless || !r.sid.len || !c->sid.len || r.sid==c->sid) {
        prettyrecord(c, t, &r);  // not a subagent's
    }
    if (c->headless) {
        return;
    }

    Json *j = r.root;
    if (r.ev == EV_REPLY) {
        i64 id = jsonint(jsonget(j, "id"), 0);
        if (!jsonint(jsonget(j, "ok"), 0) && id!=99) {
            prettyline(c);
            os_print(c->os, 2, concat(&t, concat(&t, "prospero: ",
                     jsonstr(jsonget(j, "err"))), "\n"));
            c->exitcode = 2;
            cliquit(c, t);
            return;
        }
        if ((id==1 || id==c->openid) && !c->sid.len) {
            c->sid = clone(&c->keep, r.sid);
        }
        if (id == c->openid) {
            if (c->prompt.len) {
                clicmd(c, t, CMD_PROMPT, 3, r.sid, "text", c->prompt);
            } else {
                cliquit(c, t);
            }
        }
    } else if (r.sid != c->sid) {
        return;
    } else if (r.ev == EV_TURN) {
        c->turning = 1;
    } else if (r.ev == EV_JOBSTART) {
        c->jobs++;
    } else if (r.ev == EV_JOBEND) {
        c->jobs    = c->jobs>0 ? c->jobs-1 : 0;
        c->turning = c->turning || jsonflag(jsonget(j, "resume"), 0);
        if (!c->jobs && !c->turning) {
            cliquit(c, t);
        }
    } else if (r.ev == EV_TURNEND) {
        // After a turn that went well, a turn the engine starts next to
        // report jobs is waited for, as are jobs still running
        Str why = jsonstr(jsonget(j, "why"));
        c->exitcode = exitcodefor(why);
        c->turning  = jsonflag(jsonget(j, "resume"), 0);
        if (why!="done" || (!c->jobs && !c->turning)) {
            cliquit(c, t);
        }
    }
}

// Durable records lost to a full mailbox come back from the log.
static void cliresync(Cli *c, Arena scratch)
{
    if (c->headless) {
        Buf   b(&scratch, 128);
        EvOut w = evbegin(&b, {}, 0, os_time(c->os), EV_OVERFLOW);
        os_print(c->os, 1, evend(&w));
        return;
    }
    if (!c->sid.len) {
        return;
    }
    Str path = join(&scratch, join(&scratch, join(&scratch, c->statedir,
                    "sessions"), c->sid), "events.jsonl");
    Arena  lines = newarena(c->spare);
    Replay rp    = logreplay(c->os, &lines, scratch, path, c->sid, c->lastseq, LOG_MAX);
    for (iz i = 0; i < rp.lines.len; i++) {
        clirecord(c, scratch, rp.lines[i]);
    }
}

struct StdinReader {
    Os     *os;
    Engine *e;
    Arena   a;
};

// Commands from standard input, one per line. A line longer than the
// buffer is refused by the id at its start, since every command must
// get a reply, and the rest of it skipped.
static void stdinthread(void *arg)
{
    StdinReader *r   = (StdinReader *)arg;
    iz           cap = 1 << 20;
    u8          *buf = allocbytes(&r->a, cap);
    iz           len = 0;
    b32          skip = 0;  // inside an overlong line
    for (;;) {
        iz n = os_stdin(r->os, buf+len, cap-len);
        if (n <= 0) {
            break;
        }
        len += n;
        iz start = 0;
        for (iz i = len-n; i < len; i++) {
            if (buf[i] != '\n') continue;
            Str line = trim(span(buf+start, buf+i));
            if (!skip && line.len) {
                enginecmd(r->e, line);
            }
            skip  = 0;
            start = i + 1;
        }
        for (iz i = start; i < len; i++) {
            buf[i-start] = buf[i];  // the partial line to the front
        }
        len -= start;
        if (len == cap) {
            if (!skip) {
                enginetoolong(r->e, cmdlineid({buf, len}));
            }
            len  = 0;
            skip = 1;
        }
    }
    enginecmd(r->e, "{\"cmd\":\"quit\",\"id\":0}");
}

// $XDG_STATE_HOME/prospero, else ~/.local/state/prospero (userhome),
// in the order cfgdefault looks for the configuration. On Windows,
// %LOCALAPPDATA%\prospero is used while ~/.local/state has none, as
// state went there before, or with no home at all.
static Str defaultstate(Os *os, Arena *a)
{
    Str base = os_getenv(os, a, "XDG_STATE_HOME");
    if (base.len) {
        return join(a, base, "prospero");
    }
    return homeorold(os, a, userhome(os, a), ".local/state/prospero",
                     "LOCALAPPDATA", "prospero", STAT_DIR);
}

// The configuration, its diagnostics reported on standard error: an
// explicit file must exist, and the default one is seeded on first run.
// URL includes as net says (newconfig): with CFG_NETWAIT, those with no
// cached copy are fetched first, which takes 30 s at most. Null on
// failure, reported.
static Config *cliconfig(Os *os, Arena *perm, Arena scratch, Str config,
                         Str statedir, i32 net)
{
    Str     path = config.len ? config : cfgdefault(os, perm);
    Config *cfg  = newconfig(os, perm, scratch, path, !config.len, statedir, net);
    if (!cfg) {
        os_print(os, 2, "prospero: out of memory\n");
        return 0;
    } else if (config.len && !cfg->user) {
        os_print(os, 2, "prospero: cannot read the configuration file\n");
        return 0;
    }
    for (iz i = 0; i < cfg->ini->diags.len; i++) {
        Buf b(&scratch, 256);
        print(&b, "prospero: ");
        print(&b, cfg->ini->diags[i]);
        putbyte(&b, '\n');
        os_print(os, 2, finish(&b));
    }
    return cfg;
}

static i32 climain(Os *os, Arena perm, Arena scratch, Str *args, i32 nargs, b32 headless)
{
    Cli c = {};
    c.os       = os;
    c.headless = headless;
    c.block    = -1;

    Str dir = {}, endpoint = {}, model = {}, config = {}, state = {}, resume = {};
    Str prompt = {}, role = {};
    for (i32 i = 2; i < nargs; i++) {
        Str a = args[i];
        b32 more = i+1 < nargs;
        if (a=="-C" && more) {
            dir = args[++i];
        } else if (a=="-e" && more) {
            endpoint = args[++i];
        } else if (a=="-m" && more) {
            model = args[++i];
        } else if (a=="-r" && more && !headless) {
            role = args[++i];
        } else if (a=="-c" && more) {
            config = args[++i];
        } else if (a=="--state" && more) {
            state = args[++i];
        } else if (a=="--resume" && more && !headless) {
            resume = args[++i];
        } else if (a=="--pretty" && !headless) {
            c.pretty = 1;
        } else if (!headless && !prompt.data && (!a.len || a[0]!='-')) {
            prompt = a;
        } else {
            os_print(os, 2, cliusage);
            return 2;
        }
    }
    if (!headless && !resume.len && !prompt.len) {
        os_print(os, 2, cliusage);
        return 2;
    }

    // URL includes not yet cached are fetched now, briefly
    c.statedir  = state.len ? state : defaultstate(os, &perm);
    Config *cfg = cliconfig(os, &perm, scratch, config, c.statedir, CFG_NETWAIT);
    if (!cfg) {
        return 2;
    }

    Str sessions = join(&perm, c.statedir, "sessions");
    if (!os_mkdirs(os, scratch, sessions)) {
        os_print(os, 2, "prospero: cannot create the state directory\n");
        return 2;
    }

    c.spare = os_reserve(os, sizeof(void *)==8 ? (iz)1<<32 : (iz)1<<27);
    c.out   = c.spare ? newmailbox(&perm, 4<<20, 0, 0) : 0;
    c.e   = c.out ? newengine(os, &perm, cfg, c.statedir, c.out) : 0;
    if (!c.e || !enginestart(c.e)) {
        os_print(os, 2, "prospero: cannot start the engine\n");
        return 2;
    }

    if (headless) {
        StdinReader *r = alloc<StdinReader>(&perm);
        r->os = os;
        r->e  = c.e;
        r->a  = newarena(os_reserve(os, (iz)1<<24));
        if (!r->a.beg || !os_thread(os, stdinthread, r)) {
            os_print(os, 2, "prospero: cannot read standard input\n");
            return 2;
        }
    } else if (resume.len) {
        c.openid = 2;
        c.prompt = prompt;
        Arena a = scratch;
        Buf   b(&a, 512);
        EvOut w = cmdbegin(&b, CMD_OPEN, c.openid);
        putkey(&w.json, "sid");
        putstr(&w.json, resume);
        putkey(&w.json, "from");
        putint(&w.json, -1);
        if (model.len) {
            putkey(&w.json, "model");
            putstr(&w.json, model);
        }
        if (endpoint.len) {
            putkey(&w.json, "endpoint");
            putstr(&w.json, endpoint);
        }
        enginecmd(c.e, cuttail(evend(&w), 1));
    } else {
        Arena a   = scratch;
        Str   cwd = os_realpath(os, &a, dir.len ? dir : Str("."));
        Buf   b(&a, 512 + prompt.len);
        EvOut w = cmdbegin(&b, CMD_NEW, 1);
        putkey(&w.json, "cwd");
        putstr(&w.json, cwd.len ? cwd : dir);
        putkey(&w.json, "model");
        putstr(&w.json, model);
        putkey(&w.json, "endpoint");
        putstr(&w.json, endpoint);
        if (role.len) {
            putkey(&w.json, "role");
            putstr(&w.json, role);
        }
        putkey(&w.json, "prompt");
        putstr(&w.json, prompt);
        enginecmd(c.e, cuttail(evend(&w), 1));
    }

    c.keep = perm;
    for (;;) {
        mailwait(c.out, -1);
        Mail m = mailtake(c.out);
        if (m.overflow) {
            cliresync(&c, scratch);
        }
        for (Record r; (r = mailnext(&m)).ok;) {
            if (r.type == MAIL_QUIT) {
                prettyline(&c);
                return c.exitcode;
            }
            clirecord(&c, scratch, r.data);
        }
    }
}
